#include "sink/MqttBlurSink.h"

#include <mosquitto.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

#include "log/Logger.h"

namespace {

constexpr const char* kIface = "MqttBlur";

std::string createClientId() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return "veda-compute-blur-" + std::to_string(ticks);
}

}  // namespace

MqttBlurSink::MqttBlurSink(const AppConfig& config)
    : host_(config.mqttHost),
      port_(config.mqttPort),
      caFile_(config.mqttCaFile),
      clientId_(config.mqttBlurClientId),
      keepAliveSeconds_(config.mqttKeepAliveSeconds),
      maxQueueSize_(static_cast<std::size_t>(std::max(1, config.mqttBlurMaxQueueSize))),
      channelCount_(config.channelCount),
      retryInterval_(config.mqttRetryIntervalMs) {
    if (clientId_.empty())
        clientId_ = createClientId();

    if (initialize()) {
        worker_ = std::thread(&MqttBlurSink::workerLoop, this);
    } else {
        logError(kIface,
                 "초기 연결 실패 — " + std::to_string(retryInterval_.count()) + "ms 간격으로 백그라운드 재시도함");
        retryThread_ = std::thread(&MqttBlurSink::retryLoop, this);
    }
}

MqttBlurSink::~MqttBlurSink() { shutdown(); }

void MqttBlurSink::retryLoop() noexcept {
    std::unique_lock<std::mutex> lock(retryMutex_);
    while (!shuttingDown_.load(std::memory_order_acquire)) {
        const bool stopped =
            retryCv_.wait_for(lock, retryInterval_, [this] { return shuttingDown_.load(std::memory_order_acquire); });
        if (stopped) {
            return;
        }

        lock.unlock();
        const bool connected = initialize();
        lock.lock();

        if (shuttingDown_.load(std::memory_order_acquire)) {
            // shutdown()이 이미 진행 중 -> 방금 연결에 성공했어도 워커를 새로 띄우지 않고
            // shutdown()의 나머지 절차(client_ 파괴 등)가 정리하도록 그대로 반환
            return;
        }
        if (connected) {
            worker_ = std::thread(&MqttBlurSink::workerLoop, this);
            return;
        }
        logError(kIface, "재시도 실패, " + std::to_string(retryInterval_.count()) + "ms 후 다시 시도함");
    }
}

bool MqttBlurSink::initialize() noexcept {
    if (host_.empty()) {
        logError(kIface, "MQTT host가 비어있음 (AppConfig::mqttHost 확인 필요)");
        return false;
    }

    if (port_ <= 0 || port_ > 65535) {
        logError(kIface, "잘못된 MQTT port=" + std::to_string(port_));
        return false;
    }

    int result = mosquitto_lib_init();
    if (result != MOSQ_ERR_SUCCESS) {
        logError(kIface, std::string("mosquitto_lib_init 실패: ") + mosquitto_strerror(result));
        return false;
    }
    libraryInitialized_ = true;

    client_ = mosquitto_new(clientId_.c_str(), true, this);
    if (client_ == nullptr) {
        logError(kIface, "mosquitto_new 실패");
        mosquitto_lib_cleanup();
        libraryInitialized_ = false;
        return false;
    }

    mosquitto_connect_callback_set(client_, &MqttBlurSink::onConnect);
    mosquitto_disconnect_callback_set(client_, &MqttBlurSink::onDisconnect);
    mosquitto_reconnect_delay_set(client_, 1, 10, true);

    result = mosquitto_int_option(client_, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311);

    if (result == MOSQ_ERR_SUCCESS)
        result = mosquitto_tls_set(client_, caFile_.c_str(), nullptr, nullptr, nullptr, nullptr);

    if (result == MOSQ_ERR_SUCCESS) {
        // false: 브로커 인증서의 IP/SAN을 정상적으로 검증
        result = mosquitto_tls_insecure_set(client_, false);
    }

    if (result == MOSQ_ERR_SUCCESS)
        result = mosquitto_connect_async(client_, host_.c_str(), port_, keepAliveSeconds_);

    if (result == MOSQ_ERR_SUCCESS)
        result = mosquitto_loop_start(client_);

    if (result != MOSQ_ERR_SUCCESS) {
        logError(kIface, std::string("초기화 실패: ") + mosquitto_strerror(result));
        mosquitto_destroy(client_);
        client_ = nullptr;
        mosquitto_lib_cleanup();
        libraryInitialized_ = false;
        return false;
    }

    ready_.store(true, std::memory_order_release);
    logSuccess(kIface, "연결 시도 중 (host=" + host_ + ", port=" + std::to_string(port_) +
                           ", tls=on, clientId=" + clientId_ + ")");
    return true;
}

void MqttBlurSink::shutdown() noexcept {
    bool expected = false;
    if (!shuttingDown_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    retryCv_.notify_all();
    if (retryThread_.joinable())
        retryThread_.join();

    ready_.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopping_ = true;
        while (!queue_.empty()) {
            queue_.pop_front();
            recordDrop("shutdown");
        }
    }
    queueChanged_.notify_all();

    if (worker_.joinable())
        worker_.join();

    connected_.store(false, std::memory_order_release);

    if (client_ != nullptr) {
        mosquitto_disconnect(client_);
        mosquitto_loop_stop(client_, true);
        mosquitto_destroy(client_);
        client_ = nullptr;
    }

    if (libraryInitialized_) {
        mosquitto_lib_cleanup();
        libraryInitialized_ = false;
    }
}

bool MqttBlurSink::isValidFrame(const veda::BlurFrame& frame) const noexcept {
    if (frame.v != veda::kSchemaVersion)
        return false;

    if (frame.ts <= 0)
        return false;

    if (frame.ch < 0 || frame.ch >= channelCount_)
        return false;

    return true;
}

bool MqttBlurSink::isValidBlurTarget(const veda::BlurTarget& blur) const noexcept {
    if (!veda::isBlurClass(blur.cls))
        return false;

    const auto& box = blur.box;
    if (!std::isfinite(box.l) || !std::isfinite(box.t) || !std::isfinite(box.r) || !std::isfinite(box.b))
        return false;

    if (box.l < 0.0 || box.l > 1.0 || box.t < 0.0 || box.t > 1.0 || box.r < 0.0 || box.r > 1.0 || box.b < 0.0 ||
        box.b > 1.0)
        return false;

    if (box.l > box.r || box.t > box.b)
        return false;

    return true;
}

void MqttBlurSink::send(const veda::BlurFrame& frame) noexcept {
    if (!isValidFrame(frame)) {
        recordDrop("invalid BlurFrame");
        return;
    }

    if (!ready_.load(std::memory_order_acquire)) {
        recordDrop("MQTT client not ready");
        return;
    }

    try {
        // 개별 blur 대상 중 클래스/좌표가 이상한 것만 걸러내고 나머지는 그대로 발행한다.
        // (얼굴 하나가 인식 안 되는 클래스라고 같은 프레임의 다른 blur까지 통째로 버리지 않기 위함
        // -- blurs가 비어 있는 프레임도 정상: 이전 프레임의 blur 영역을 지우려면 빈 프레임도 필요)
        veda::BlurFrame filtered;
        filtered.v = frame.v;
        filtered.ts = frame.ts;
        filtered.ch = frame.ch;
        filtered.blurs.reserve(frame.blurs.size());

        for (const auto& blur : frame.blurs) {
            if (isValidBlurTarget(blur)) {
                filtered.blurs.push_back(blur);
            } else {
                recordDrop("invalid blur target skipped");
            }
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex_);

            if (stopping_) {
                recordDrop("sink is stopping");
                return;
            }

            // 실시간 좌표이므로 큐가 꽉 차면 가장 오래된 프레임을 버림
            if (queue_.size() >= maxQueueSize_) {
                queue_.pop_front();
                recordDrop("queue full; oldest frame removed");
            }

            queue_.push_back(std::move(filtered));
        }

        queueChanged_.notify_one();
    } catch (const std::exception& error) {
        recordDrop(error.what());
    } catch (...) {
        recordDrop("unknown enqueue exception");
    }
}

void MqttBlurSink::workerLoop() noexcept {
    while (true) {
        veda::BlurFrame frame;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);

            queueChanged_.wait(
                lock, [this] { return stopping_ || (connected_.load(std::memory_order_acquire) && !queue_.empty()); });

            if (stopping_)
                return;

            frame = std::move(queue_.front());
            queue_.pop_front();
        }

        publishFrame(frame);
    }
}

void MqttBlurSink::publishFrame(const veda::BlurFrame& frame) noexcept {
    try {
        const std::string topic = veda::topic::blur(frame.ch);
        const std::string payload = veda::encode(frame);

        if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            recordDrop("payload too large");
            return;
        }

        const int result = mosquitto_publish(client_, nullptr, topic.c_str(), static_cast<int>(payload.size()),
                                             payload.data(), veda::qos::kBlur, false);

        if (result != MOSQ_ERR_SUCCESS) {
            recordDrop(mosquitto_strerror(result));
            return;
        }

        const std::uint64_t count = publishedCount_.fetch_add(1, std::memory_order_relaxed) + 1;

        // payload 전체는 CSV에 넣기엔 부담스러워서 크기/개수만 요약
        logSuccess(kIface, "발행 성공 #" + std::to_string(count) + " topic=" + topic +
                               " ch=" + std::to_string(frame.ch) + " blurs=" + std::to_string(frame.blurs.size()) +
                               " bytes=" + std::to_string(payload.size()));
    } catch (const std::exception& error) {
        recordDrop(error.what());
    } catch (...) {
        recordDrop("unknown publish exception");
    }
}

void MqttBlurSink::recordDrop(const char* reason) noexcept {
    const std::uint64_t count = droppedCount_.fetch_add(1, std::memory_order_relaxed) + 1;

    if (count == 1 || count % 100 == 0) {
        logError(kIface, "드랍 누적 " + std::to_string(count) +
                             "건, 사유=" + std::string(reason != nullptr ? reason : "unknown"));
    }
}

void MqttBlurSink::onConnect(struct mosquitto*, void* userData, int resultCode) {
    auto* self = static_cast<MqttBlurSink*>(userData);
    if (self == nullptr)
        return;

    const bool connected = resultCode == MOSQ_ERR_SUCCESS;
    self->connected_.store(connected, std::memory_order_release);

    if (connected) {
        logSuccess(kIface, "연결 성공 (host=" + self->host_ + ", port=" + std::to_string(self->port_) + ")");
        self->queueChanged_.notify_all();
    } else {
        logError(kIface, "연결 실패: rc=" + std::to_string(resultCode) + " " + mosquitto_connack_string(resultCode));
    }
}

void MqttBlurSink::onDisconnect(struct mosquitto*, void* userData, int resultCode) {
    auto* self = static_cast<MqttBlurSink*>(userData);
    if (self == nullptr)
        return;

    self->connected_.store(false, std::memory_order_release);

    if (!self->shuttingDown_.load(std::memory_order_acquire)) {
        logError(kIface, "연결 끊김: rc=" + std::to_string(resultCode) + " " + mosquitto_strerror(resultCode));
    }
}

bool MqttBlurSink::isReady() const noexcept { return ready_.load(std::memory_order_acquire); }

bool MqttBlurSink::isConnected() const noexcept { return connected_.load(std::memory_order_acquire); }

std::uint64_t MqttBlurSink::publishedCount() const noexcept { return publishedCount_.load(std::memory_order_relaxed); }

std::uint64_t MqttBlurSink::droppedCount() const noexcept { return droppedCount_.load(std::memory_order_relaxed); }
