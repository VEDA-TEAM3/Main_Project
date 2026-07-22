#include "sink/MqttSink.h"

#include <mosquitto.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace {

constexpr int kChannelCount = 4;
constexpr int kDefaultMqttPort = 8883;

std::string readEnvironment(
    const char* name,
    const char* fallback
) {
    const char* value = std::getenv(name);

    if (value == nullptr || *value == '\0') {
        return fallback;
    }

    return value;
}

int readMqttPort() noexcept {
    const char* value = std::getenv("VEDA_MQTT_PORT");

    if (value == nullptr || *value == '\0') {
        return kDefaultMqttPort;
    }

    try {
        const int port = std::stoi(value);

        if (port > 0 && port <= 65535) {
            return port;
        }
    } catch (...) {
    }

    std::fprintf(
        stderr,
        "[MqttBlurSink] invalid VEDA_MQTT_PORT=%s; "
        "using default port=%d\n",
        value,
        kDefaultMqttPort
    );

    return kDefaultMqttPort;
}

std::string createClientId() {
    const auto ticks =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    return
        "veda-compute-blur-" +
        std::to_string(ticks);
}

MqttBlurSink::Config createDefaultConfig() {
    MqttBlurSink::Config config;

    config.host = readEnvironment(
        "VEDA_MQTT_HOST",
        "172.20.27.174"
    );

    config.port = readMqttPort();

    config.caFile = readEnvironment(
        "VEDA_MQTT_CA_FILE",
        "/etc/veda/certs/ca.crt"
    );

    config.clientId = readEnvironment(
        "VEDA_MQTT_CLIENT_ID",
        ""
    );

    if (config.clientId.empty()) {
        config.clientId = createClientId();
    }

    return config;
}

/**
 * 프로세스 전체에서 MQTT 연결 하나만 사용한다.
 */
MqttBlurSink& sharedBlurSink() {
    static MqttBlurSink sink(createDefaultConfig());
    return sink;
}

}  // namespace

MqttBlurSink::MqttBlurSink(Config config)
    : config_(std::move(config)) {
    config_.maxQueueSize = std::max<std::size_t>(
        1,
        config_.maxQueueSize
    );

    if (config_.clientId.empty()) {
        config_.clientId = createClientId();
    }

    if (initialize()) {
        worker_ = std::thread(
            &MqttBlurSink::workerLoop,
            this
        );
    }
}

MqttBlurSink::~MqttBlurSink() noexcept {
    shutdown();
}

bool MqttBlurSink::initialize() noexcept {
    if (config_.host.empty()) {
        std::fprintf(
            stderr,
            "[MqttBlurSink] MQTT host is empty\n"
        );
        return false;
    }

    if (config_.port <= 0 || config_.port > 65535) {
        std::fprintf(
            stderr,
            "[MqttBlurSink] invalid MQTT port=%d\n",
            config_.port
        );
        return false;
    }

    int result = mosquitto_lib_init();

    if (result != MOSQ_ERR_SUCCESS) {
        std::fprintf(
            stderr,
            "[MqttBlurSink] mosquitto_lib_init failed: %s\n",
            mosquitto_strerror(result)
        );
        return false;
    }

    libraryInitialized_ = true;

    client_ = mosquitto_new(
        config_.clientId.c_str(),
        true,
        this
    );

    if (client_ == nullptr) {
        std::fprintf(
            stderr,
            "[MqttBlurSink] mosquitto_new failed\n"
        );

        mosquitto_lib_cleanup();
        libraryInitialized_ = false;

        return false;
    }

    mosquitto_connect_callback_set(
        client_,
        &MqttBlurSink::onConnect
    );

    mosquitto_disconnect_callback_set(
        client_,
        &MqttBlurSink::onDisconnect
    );

    mosquitto_reconnect_delay_set(
        client_,
        1,
        10,
        true
    );

    result = mosquitto_int_option(
        client_,
        MOSQ_OPT_PROTOCOL_VERSION,
        MQTT_PROTOCOL_V311
    );

    if (result == MOSQ_ERR_SUCCESS) {
        result = mosquitto_tls_set(
            client_,
            config_.caFile.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr
        );
    }

    if (result == MOSQ_ERR_SUCCESS) {
        /*
         * false:
         * broker 인증서의 IP/SAN을 정상적으로 검증한다.
         */
        result = mosquitto_tls_insecure_set(
            client_,
            false
        );
    }

    if (result == MOSQ_ERR_SUCCESS) {
        result = mosquitto_connect_async(
            client_,
            config_.host.c_str(),
            config_.port,
            config_.keepAliveSeconds
        );
    }

    if (result == MOSQ_ERR_SUCCESS) {
        result = mosquitto_loop_start(client_);
    }

    if (result != MOSQ_ERR_SUCCESS) {
        std::fprintf(
            stderr,
            "[MqttBlurSink] initialization failed: %s\n",
            mosquitto_strerror(result)
        );

        mosquitto_destroy(client_);
        client_ = nullptr;

        mosquitto_lib_cleanup();
        libraryInitialized_ = false;

        return false;
    }

    ready_.store(
        true,
        std::memory_order_release
    );

    std::fprintf(
        stdout,
        "[MqttBlurSink] connecting: "
        "host=%s port=%d tls=on clientId=%s\n",
        config_.host.c_str(),
        config_.port,
        config_.clientId.c_str()
    );

    return true;
}

void MqttBlurSink::shutdown() noexcept {
    bool expected = false;

    if (!shuttingDown_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    ready_.store(
        false,
        std::memory_order_release
    );

    {
        std::lock_guard lock(queueMutex_);

        stopping_ = true;

        while (!queue_.empty()) {
            queue_.pop_front();
            recordDrop("shutdown");
        }
    }

    queueChanged_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }

    connected_.store(
        false,
        std::memory_order_release
    );

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

bool MqttBlurSink::isValidFrame(
    const veda::BlurFrame& frame
) noexcept {
    if (frame.v != veda::kSchemaVersion) {
        return false;
    }

    if (frame.ts <= 0) {
        return false;
    }

    if (frame.ch < 0 || frame.ch >= kChannelCount) {
        return false;
    }

    for (const auto& blur : frame.blurs) {
        if (!veda::isBlurClass(blur.cls)) {
            return false;
        }

        const auto& box = blur.box;

        if (!std::isfinite(box.l) ||
            !std::isfinite(box.t) ||
            !std::isfinite(box.r) ||
            !std::isfinite(box.b)) {
            return false;
        }

        if (box.l < 0.0 || box.l > 1.0 ||
            box.t < 0.0 || box.t > 1.0 ||
            box.r < 0.0 || box.r > 1.0 ||
            box.b < 0.0 || box.b > 1.0) {
            return false;
        }

        if (box.l > box.r || box.t > box.b) {
            return false;
        }
    }

    /*
     * blurs가 비어 있는 프레임도 정상이다.
     * 이전 프레임의 blur 영역을 제거하려면 빈 프레임도 필요하다.
     */
    return true;
}

void MqttBlurSink::send(
    const veda::BlurFrame& frame
) noexcept {
    if (!isValidFrame(frame)) {
        recordDrop("invalid BlurFrame");
        return;
    }

    if (!ready_.load(std::memory_order_acquire)) {
        recordDrop("MQTT client not ready");
        return;
    }

    try {
        {
            std::lock_guard lock(queueMutex_);

            if (stopping_) {
                recordDrop("sink is stopping");
                return;
            }

            /*
             * 실시간 좌표이므로 queue가 꽉 차면
             * 가장 오래된 프레임을 버린다.
             */
            if (queue_.size() >= config_.maxQueueSize) {
                queue_.pop_front();
                recordDrop("queue full; oldest frame removed");
            }

            queue_.push_back(frame);
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
            std::unique_lock lock(queueMutex_);

            queueChanged_.wait(
                lock,
                [this] {
                    return stopping_ ||
                           (
                               connected_.load(
                                   std::memory_order_acquire
                               ) &&
                               !queue_.empty()
                           );
                }
            );

            if (stopping_) {
                return;
            }

            frame = std::move(queue_.front());
            queue_.pop_front();
        }

        publishFrame(frame);
    }
}

void MqttBlurSink::publishFrame(
    const veda::BlurFrame& frame
) noexcept {
    try {
        const std::string topic =
            veda::topic::blur(frame.ch);

        const std::string payload =
            veda::encode(frame);

        if (payload.size() >
            static_cast<std::size_t>(
                std::numeric_limits<int>::max()
            )) {
            recordDrop("payload too large");
            return;
        }

        const int result = mosquitto_publish(
            client_,
            nullptr,
            topic.c_str(),
            static_cast<int>(payload.size()),
            payload.data(),
            veda::qos::kBlur,
            false
        );

        if (result != MOSQ_ERR_SUCCESS) {
            recordDrop(mosquitto_strerror(result));
            return;
        }

        const std::uint64_t count =
            publishedCount_.fetch_add(
                1,
                std::memory_order_relaxed
            ) + 1;

        std::fprintf(
            stdout,
            "[MQTT BLUR PUB] "
            "count=%llu topic=%s ts=%lld blurs=%zu "
            "payload=%s\n",
            static_cast<unsigned long long>(count),
            topic.c_str(),
            static_cast<long long>(frame.ts),
            frame.blurs.size(),
            payload.c_str()
        );

        std::fflush(stdout);
    } catch (const std::exception& error) {
        recordDrop(error.what());
    } catch (...) {
        recordDrop("unknown publish exception");
    }
}

void MqttBlurSink::recordDrop(
    const char* reason
) noexcept {
    const std::uint64_t count =
        droppedCount_.fetch_add(
            1,
            std::memory_order_relaxed
        ) + 1;

    if (count == 1 || count % 100 == 0) {
        std::fprintf(
            stderr,
            "[MqttBlurSink] dropped=%llu reason=%s\n",
            static_cast<unsigned long long>(count),
            reason != nullptr ? reason : "unknown"
        );
    }
}

void MqttBlurSink::onConnect(
    struct mosquitto*,
    void* userData,
    int resultCode
) {
    auto* self =
        static_cast<MqttBlurSink*>(userData);

    if (self == nullptr) {
        return;
    }

    const bool connected =
        resultCode == MOSQ_ERR_SUCCESS;

    self->connected_.store(
        connected,
        std::memory_order_release
    );

    if (connected) {
        std::fprintf(
            stdout,
            "[MqttBlurSink] connected: "
            "host=%s port=%d\n",
            self->config_.host.c_str(),
            self->config_.port
        );

        std::fflush(stdout);

        self->queueChanged_.notify_all();
    } else {
        std::fprintf(
            stderr,
            "[MqttBlurSink] connect failed: rc=%d %s\n",
            resultCode,
            mosquitto_connack_string(resultCode)
        );
    }
}

void MqttBlurSink::onDisconnect(
    struct mosquitto*,
    void* userData,
    int resultCode
) {
    auto* self =
        static_cast<MqttBlurSink*>(userData);

    if (self == nullptr) {
        return;
    }

    self->connected_.store(
        false,
        std::memory_order_release
    );

    if (!self->shuttingDown_.load(
            std::memory_order_acquire)) {
        std::fprintf(
            stderr,
            "[MqttBlurSink] disconnected: rc=%d %s\n",
            resultCode,
            mosquitto_strerror(resultCode)
        );
    }
}

bool MqttBlurSink::isReady() const noexcept {
    return ready_.load(std::memory_order_acquire);
}

bool MqttBlurSink::isConnected() const noexcept {
    return connected_.load(std::memory_order_acquire);
}

std::uint64_t MqttBlurSink::publishedCount() const noexcept {
    return publishedCount_.load(
        std::memory_order_relaxed
    );
}

std::uint64_t MqttBlurSink::droppedCount() const noexcept {
    return droppedCount_.load(
        std::memory_order_relaxed
    );
}

void publishBlurToMqtt(
    const veda::BlurFrame& frame
) noexcept {
    try {
        sharedBlurSink().send(frame);
    } catch (const std::exception& error) {
        std::fprintf(
            stderr,
            "[MqttBlurSink] publish entry failed: %s\n",
            error.what()
        );
    } catch (...) {
        std::fprintf(
            stderr,
            "[MqttBlurSink] publish entry failed: "
            "unknown exception\n"
        );
    }
}