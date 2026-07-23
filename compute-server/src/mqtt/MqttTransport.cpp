#include "mqtt/MqttTransport.h"

#include <mosquitto.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

#include "Logger.h"

namespace {

constexpr const char* kIface = "MqttTransport";

/// @name mosquitto 전역 라이브러리 참조 카운트
/// @details lib_init/lib_cleanup 은 전역 상태를 다루므로 마지막 사용자가 사라질 때만 정리
/// @{
std::mutex g_libraryMutex;
std::size_t g_libraryRefCount = 0;

bool acquireMosquittoLibrary() noexcept {
    std::lock_guard<std::mutex> lock(g_libraryMutex);
    if (g_libraryRefCount == 0 && mosquitto_lib_init() != MOSQ_ERR_SUCCESS)
        return false;
    ++g_libraryRefCount;
    return true;
}

void releaseMosquittoLibrary() noexcept {
    std::lock_guard<std::mutex> lock(g_libraryMutex);
    if (g_libraryRefCount == 0)
        return;
    --g_libraryRefCount;
    if (g_libraryRefCount == 0)
        mosquitto_lib_cleanup();
}
/// @}

std::string createClientId() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return "veda-compute-" + std::to_string(ticks);
}

/// @name 채널 생존 신호 페이로드 (Contract.h: 한 바이트 "1"=alive, "0"=dead)
/// @{
constexpr std::string_view kAlivePayload = "1";
constexpr std::string_view kDeadPayload = "0";
/// @}

}  // namespace

MqttTransport::MqttTransport(const AppConfig& config)
    : host_(config.mqttHost),
      port_(config.mqttPort),
      caFile_(config.mqttCaFile),
      clientId_(config.mqttClientId),
      keepAliveSeconds_(config.mqttKeepAliveSeconds),
      retryInterval_(config.mqttRetryIntervalMs),
      reconnectDelaySec_(config.mqttReconnectDelaySec),
      reconnectDelayMaxSec_(config.mqttReconnectDelayMaxSec),
      aliveTopic_(veda::topic::alive(config.channelId)) {
    if (clientId_.empty())
        clientId_ = createClientId();
}

MqttTransport::~MqttTransport() { stop(); }

MqttTransport::ListenerId MqttTransport::addConnectionListener(std::function<void(bool)> listener) {
    if (!listener)
        return kInvalidListener;

    std::lock_guard<std::mutex> lock(listenerMutex_);
    const ListenerId id = nextListenerId_++;
    connectionListeners_.push_back(Listener{id, std::move(listener)});
    return id;
}

void MqttTransport::removeConnectionListener(ListenerId id) noexcept {
    if (id == kInvalidListener)
        return;

    // notifyListeners()가 콜백 실행 내내 listenerMutex_ 를 잡고 있으므로,
    // 여기서 락을 얻었다는 건 그 콜백이 실행 중이 아니라는 뜻
    std::lock_guard<std::mutex> lock(listenerMutex_);
    connectionListeners_.erase(std::remove_if(connectionListeners_.begin(), connectionListeners_.end(),
                                              [id](const Listener& listener) { return listener.id == id; }),
                               connectionListeners_.end());
}

bool MqttTransport::start() noexcept {
    if (initializeClient())
        return true;

    logError(kIface, "초기 연결 실패 — " + std::to_string(retryInterval_.count()) + "ms 간격으로 백그라운드 재시도함");
    retryThread_ = std::thread(&MqttTransport::retryLoop, this);
    return false;
}

void MqttTransport::retryLoop() noexcept {
    std::unique_lock<std::mutex> lock(retryMutex_);
    while (!stopping_.load(std::memory_order_acquire)) {
        const bool stopped =
            retryCv_.wait_for(lock, retryInterval_, [this] { return stopping_.load(std::memory_order_acquire); });
        if (stopped)
            return;

        lock.unlock();
        const bool connected = initializeClient();
        lock.lock();

        // stop()이 이미 진행 중이면 방금 만든 클라이언트도 stop()의 절차가 정리하도록 두고 빠짐
        if (stopping_.load(std::memory_order_acquire) || connected)
            return;

        logError(kIface, "재시도 실패, " + std::to_string(retryInterval_.count()) + "ms 후 다시 시도함");
    }
}

bool MqttTransport::initializeClient() noexcept {
    if (host_.empty()) {
        logError(kIface, "MQTT host가 비어있음 (AppConfig::mqttHost 확인 필요)");
        return false;
    }
    if (port_ <= 0 || port_ > 65535) {
        logError(kIface, "잘못된 MQTT port=" + std::to_string(port_));
        return false;
    }
    if (caFile_.empty()) {
        logError(kIface, "TLS CA 파일 경로가 비어있음 (AppConfig::mqttCaFile 확인 필요)");
        return false;
    }

    if (!acquireMosquittoLibrary()) {
        logError(kIface, "mosquitto_lib_init 실패");
        return false;
    }
    libraryAcquired_ = true;

    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        client_ = mosquitto_new(clientId_.c_str(), true, this);
    }
    if (client_ == nullptr) {
        logError(kIface, "mosquitto_new 실패");
        destroyClient();
        return false;
    }

    mosquitto_connect_callback_set(client_, &MqttTransport::onConnect);
    mosquitto_disconnect_callback_set(client_, &MqttTransport::onDisconnect);
    mosquitto_reconnect_delay_set(client_, static_cast<unsigned int>(reconnectDelaySec_),
                                  static_cast<unsigned int>(reconnectDelayMaxSec_), true);

    // LWT: 접속 '이전'에 걸어야 브로커가 유언으로 등록함 (접속 후에는 늦음)
    int result = mosquitto_will_set(client_, aliveTopic_.c_str(), static_cast<int>(kDeadPayload.size()),
                                    kDeadPayload.data(), veda::qos::kAlive, true);
    if (result != MOSQ_ERR_SUCCESS)
        logError(kIface, std::string("LWT 설정 실패: ") + mosquitto_strerror(result));

    if (result == MOSQ_ERR_SUCCESS)
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
        destroyClient();
        return false;
    }

    ready_.store(true, std::memory_order_release);
    logSuccess(kIface, "연결 시도 중 (host=" + host_ + ", port=" + std::to_string(port_) +
                           ", tls=on, clientId=" + clientId_ + ")");
    return true;
}

void MqttTransport::destroyClient() noexcept {
    mosquitto* client = nullptr;
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        client = client_;
        client_ = nullptr;
    }

    if (client != nullptr) {
        mosquitto_disconnect(client);
        mosquitto_loop_stop(client, true);
        mosquitto_destroy(client);
    }

    ready_.store(false, std::memory_order_release);
    connected_.store(false, std::memory_order_release);

    if (libraryAcquired_) {
        releaseMosquittoLibrary();
        libraryAcquired_ = false;
    }
}

void MqttTransport::stop() noexcept {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    retryCv_.notify_all();
    if (retryThread_.joinable())
        retryThread_.join();

    // 정상 종료: 유언(LWT)을 기다리지 않고 직접 "0"을 남김
    // -- LWT는 브로커가 keepalive 만료를 감지해야 발행되므로 최대 1.5*keepalive 만큼 늦음
    //    깨끗한 종료에서는 control-server가 즉시 알 수 있도록 먼저 보냄
    if (connected_.load(std::memory_order_acquire)) {
        if (publish(aliveTopic_, kDeadPayload, veda::qos::kAlive, true))
            logSuccess(kIface, "종료 신호 발행 (topic=" + aliveTopic_ + ")");
    }

    destroyClient();

    // 종료 중임을 알려 Sink 워커들이 대기에서 깨어나도록 함
    notifyListeners(false);
}

bool MqttTransport::publish(std::string_view topic, std::string_view payload, int qos, bool retain) noexcept {
    if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;

    std::lock_guard<std::mutex> lock(clientMutex_);
    if (client_ == nullptr)
        return false;

    // topic 은 C 문자열이 필요하므로 여기서만 std::string 으로 만듦
    const std::string topicString(topic);
    const int result = mosquitto_publish(client_, nullptr, topicString.c_str(), static_cast<int>(payload.size()),
                                         payload.data(), qos, retain);
    return result == MOSQ_ERR_SUCCESS;
}

void MqttTransport::notifyListeners(bool connected) noexcept {
    // 목록을 복사해서 락 밖에서 부르지 않고, 락을 잡은 채로 호출함
    // -- 복사 후 호출하면 그 사이에 Sink 가 파괴되어 죽은 객체를 부를 수 있음
    //    (removeConnectionListener() 의 "반환 시 실행 중 아님" 보장이 여기에 달려 있음)
    // 리스너는 조건변수 notify 정도만 하므로 락 유지 시간은 짧음
    std::lock_guard<std::mutex> lock(listenerMutex_);
    for (const auto& listener : connectionListeners_) {
        try {
            listener.callback(connected);
        } catch (...) {
            logError(kIface, "연결 상태 리스너에서 예외 발생 (무시함)");
        }
    }
}

void MqttTransport::onConnect(mosquitto*, void* userData, int resultCode) {
    auto* self = static_cast<MqttTransport*>(userData);
    if (self == nullptr)
        return;

    const bool connected = resultCode == MOSQ_ERR_SUCCESS;
    self->connected_.store(connected, std::memory_order_release);

    if (connected) {
        logSuccess(kIface, "연결 성공 (host=" + self->host_ + ", port=" + std::to_string(self->port_) + ")");

        // 재접속 때마다 다시 보냄: 브로커에 남아 있던 retained "0"(유언)을 덮어써야
        // control-server 가 이 채널을 되살아난 것으로 인식함
        if (!self->publish(self->aliveTopic_, kAlivePayload, veda::qos::kAlive, true))
            logError(kIface, "생존 신호 발행 실패 (topic=" + self->aliveTopic_ + ")");
    } else {
        logError(kIface, "연결 실패: rc=" + std::to_string(resultCode) + " " + mosquitto_connack_string(resultCode));
    }

    self->notifyListeners(connected);
}

void MqttTransport::onDisconnect(mosquitto*, void* userData, int resultCode) {
    auto* self = static_cast<MqttTransport*>(userData);
    if (self == nullptr)
        return;

    self->connected_.store(false, std::memory_order_release);

    if (!self->stopping_.load(std::memory_order_acquire))
        logError(kIface, "연결 끊김: rc=" + std::to_string(resultCode) + " " + mosquitto_strerror(resultCode));

    self->notifyListeners(false);
}
