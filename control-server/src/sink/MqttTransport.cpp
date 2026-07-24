#include "sink/MqttTransport.h"

#include <mosquitto.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

#include "Logger.h"
#include "core/AppConfig.h"

namespace {

constexpr const char* kIface = "MqttTransport";

std::string createClientId() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return "veda-control-publisher-" + std::to_string(ticks);
}

std::mutex libraryMutex;
std::size_t libraryReferenceCount = 0;

bool acquireMosquittoLibrary() noexcept {
    std::lock_guard lock(libraryMutex);
    if (libraryReferenceCount == 0 && mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
        return false;
    }
    ++libraryReferenceCount;
    return true;
}

void releaseMosquittoLibrary() noexcept {
    std::lock_guard lock(libraryMutex);
    if (libraryReferenceCount == 0) {
        return;
    }
    --libraryReferenceCount;
    if (libraryReferenceCount == 0) {
        mosquitto_lib_cleanup();
    }
}

bool isValidQos(int qos) noexcept { return qos >= 0 && qos <= 2; }

/// @brief AppConfig::mqttBrokerUrl 파싱 결과 (host/port/TLS 여부)
struct ParsedBroker {
    std::string host = "172.20.27.174";
    int port = 8883;
    bool useTls = true;
};

ParsedBroker parseBrokerUrl(const std::string& brokerUrl) {
    ParsedBroker parsed;

    std::string address = brokerUrl;
    constexpr std::string_view tcpPrefix = "tcp://";
    constexpr std::string_view mqttPrefix = "mqtt://";
    constexpr std::string_view sslPrefix = "ssl://";
    constexpr std::string_view mqttsPrefix = "mqtts://";

    if (address.starts_with(tcpPrefix)) {
        address.erase(0, tcpPrefix.size());
        parsed.useTls = false;
    } else if (address.starts_with(mqttPrefix)) {
        address.erase(0, mqttPrefix.size());
        parsed.useTls = false;
    } else if (address.starts_with(sslPrefix)) {
        address.erase(0, sslPrefix.size());
        parsed.useTls = true;
    } else if (address.starts_with(mqttsPrefix)) {
        address.erase(0, mqttsPrefix.size());
        parsed.useTls = true;
    }

    const std::size_t separator = address.rfind(':');
    if (separator != std::string::npos && separator + 1 < address.size()) {
        int port = 0;
        const char* begin = address.data() + separator + 1;
        const char* end = address.data() + address.size();
        const auto [parsedEnd, error] = std::from_chars(begin, end, port);
        if (error == std::errc{} && parsedEnd == end && port > 0 && port <= 65535) {
            parsed.port = port;
            address.resize(separator);
        }
    }
    if (!address.empty()) {
        parsed.host = std::move(address);
    }
    return parsed;
}

/// @brief WorldFrame 을 재사용 RiskFrame 버퍼(out)로 변환. out.objects 의 capacity 를 재사용해
///        프레임마다의 벡터 재할당을 없앤다 (send() 파이프라인 단일 스레드에서만 호출됨).
///        재사용 버퍼라 v/ts/level 은 매번 명시적으로 초기화해야 이전 프레임 값이 새지 않는다.
///
/// [Rule 4] 프레임 전체 위험도(out.level)는 여기서 재계산하지 않고 ThresholdRiskPolicy 가
///          zoneLevels(=HW 로 나가는 값)의 max 로 확정해 실어준 frame.level 을 그대로 읽는다.
///          -> UI 로 나가는 전체 위험도와 HW 채널 위험도가 항상 같은 소스에서 파생된다.
void fillRiskFrame(const domain::WorldFrame& frame, veda::RiskFrame& out) {
    out.v = veda::kSchemaVersion;
    out.ts = frame.timestamp;
    out.level = frame.level;  // 단일 진실 공급원: 위험 정책이 확정한 값 (재계산 금지)
    out.objects.clear();      // capacity 유지 -> warmup 이후 재할당 없음
    out.objects.reserve(frame.objects.size());

    for (const domain::WorldObject& source : frame.objects) {
        veda::RiskObject object;
        object.gid = source.gid;
        object.cls = source.cls;
        object.pos = veda::WorldPoint{source.pos.x, source.pos.y};
        object.level = source.riskLevel;
        object.nearest = source.nearestObj;
        object.dist = source.nearestDist;
        out.objects.push_back(object);
    }
}

}  // namespace

MqttTransport::MqttTransport(const AppConfig& config)
    : clientId_(config.mqttClientId),
      keepAliveSeconds_(config.mqttKeepAliveSeconds),
      reconnectDelaySeconds_(config.mqttReconnectDelaySeconds),
      reconnectDelayMaxSeconds_(config.mqttReconnectDelayMaxSeconds),
      caFile_(config.mqttCaFile),
      publishTopic_(config.mqttSendTopic.empty() ? std::string(veda::topic::kRisk) : config.mqttSendTopic) {
    if (clientId_.empty())
        clientId_ = createClientId();

    const ParsedBroker broker = parseBrokerUrl(config.mqttBrokerUrl);
    host_ = broker.host;
    port_ = broker.port;
    useTls_ = broker.useTls;
}

MqttTransport::~MqttTransport() noexcept { stop(); }

void MqttTransport::setMessageHandler(MessageHandler handler) {
    std::lock_guard lock(callbackMutex_);
    messageHandler_ = std::move(handler);
}

void MqttTransport::setConnectionHandler(ConnectionHandler handler) {
    std::lock_guard lock(callbackMutex_);
    connectionHandler_ = std::move(handler);
}

bool MqttTransport::subscribe(std::string topic, int qos) noexcept {
    if (topic.empty() || !isValidQos(qos)) {
        return false;
    }

    const std::string subscriptionTopic = topic;
    {
        std::lock_guard lock(subscriptionMutex_);
        const auto existing = std::find_if(subscriptions_.begin(), subscriptions_.end(),
                                           [&topic](const Subscription& value) { return value.topic == topic; });
        if (existing == subscriptions_.end()) {
            subscriptions_.push_back(Subscription{std::move(topic), qos});
        } else {
            existing->qos = qos;
        }
    }

    if (!isConnected()) {
        return true;
    }

    std::lock_guard lock(clientMutex_);
    if (client_ == nullptr || !isConnected()) {
        return true;
    }
    const int result = mosquitto_subscribe(client_, nullptr, subscriptionTopic.c_str(), qos);
    if (result != MOSQ_ERR_SUCCESS) {
        logError(kIface, "subscribe 실패: topic=" + subscriptionTopic + " error=" + mosquitto_strerror(result));
        return false;
    }
    return true;
}

bool MqttTransport::start() noexcept {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return true;
    }

    if (!initializeClient()) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    const int connectResult = mosquitto_connect_async(client_, host_.c_str(), port_, keepAliveSeconds_);
    if (connectResult != MOSQ_ERR_SUCCESS) {
        logError(kIface, "connect 준비 실패: host=" + host_ + " port=" + std::to_string(port_) +
                             " error=" + mosquitto_strerror(connectResult));
        running_.store(false, std::memory_order_release);
        destroyClient();
        return false;
    }

    const int loopResult = mosquitto_loop_start(client_);
    if (loopResult != MOSQ_ERR_SUCCESS) {
        logError(kIface, std::string("네트워크 loop 시작 실패: ") + mosquitto_strerror(loopResult));
        running_.store(false, std::memory_order_release);
        destroyClient();
        return false;
    }

    logSuccess(kIface, "연결 시도 중 (host=" + host_ + ", port=" + std::to_string(port_) +
                           ", tls=" + (useTls_ ? "on" : "off") + ", clientId=" + clientId_ + ")");
    return true;
}

void MqttTransport::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    connected_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(clientMutex_);
        if (client_ != nullptr) {
            mosquitto_disconnect(client_);
        }
    }

    if (client_ != nullptr) {
        mosquitto_loop_stop(client_, true);
    }
    destroyClient();
}

bool MqttTransport::publish(std::string_view topic, std::string_view payload, int qos, bool retain) noexcept {
    if (topic.empty() || !isValidQos(qos) ||
        payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    std::lock_guard lock(clientMutex_);
    if (!isRunning() || !isConnected() || client_ == nullptr) {
        return false;
    }

    const std::string topicString(topic);
    const int result = mosquitto_publish(client_, nullptr, topicString.c_str(), static_cast<int>(payload.size()),
                                         payload.data(), qos, retain);
    if (result != MOSQ_ERR_SUCCESS) {
        logError(kIface, "발행 실패: topic=" + topicString + " error=" + mosquitto_strerror(result));
        return false;
    }
    return true;
}

void MqttTransport::send(const domain::WorldFrame& frame) {
    try {
        if (frame.timestamp <= 0) {
            logError(kIface, "잘못된 WorldFrame timestamp");
            return;
        }
        if (!isRunning() && !start()) {
            return;
        }
        if (!isConnected()) {
            return;
        }

        // Zero-DOM: WorldFrame -> 재사용 RiskFrame(riskScratch_) -> 재사용 버퍼(riskPayloadBuf_)에
        // 직접 직렬화. nlohmann DOM 을 프레임마다 만들지 않으므로 warmup 이후 힙 할당이 없음.
        // riskScratch_/riskPayloadBuf_ 는 send()(파이프라인 단일 스레드)에서만 접근하므로 락 불필요.
        fillRiskFrame(frame, riskScratch_);
        veda::encodeInto(riskScratch_, riskPayloadBuf_);
        publish(publishTopic_, riskPayloadBuf_, veda::qos::kRisk, false);
    } catch (const std::exception& error) {
        logError(kIface, std::string("WorldFrame 발행 실패: ") + error.what());
    } catch (...) {
        logError(kIface, "WorldFrame 발행 실패: 알 수 없는 예외");
    }
}

void MqttTransport::sendChannelStatus(const veda::ChannelStatus& status) {
    try {
        // 상태 전환 이벤트라 호출 빈도가 낮음 -- RiskFrame(send())과 달리 rate-limit 없이
        // 모든 분기에서 로그를 남겨도 도배되지 않음. 여기서 로그를 안 남기면 "연결이 안 되어
        // 조용히 스킵됨"과 "정상 발행됨"을 로그만으로 구분할 수 없어서 원인 추적이 불가능해짐
        const std::string label = "ch=" + std::to_string(status.ch) +
                                  " cameraAlive=" + (status.cameraAlive ? "true" : "false") +
                                  " hardwareAlive=" + (status.hardwareAlive ? "true" : "false");

        if (!isRunning() && !start()) {
            logError(kIface, "ChannelStatus 발행 실패 - transport 시작 안 됨 (" + label + ")");
            return;
        }
        if (!isConnected()) {
            // 연결이 끊긴 동안의 전환은 발행되지 않지만, retained 라 브로커에는 직전 값이
            // 그대로 남아있으므로 재연결 후 다음 상태 전환이 오면 최신화됨 (RiskFrame과 동일 정책)
            logError(kIface, "ChannelStatus 발행 안 함 - MQTT 미연결 (" + label + ")");
            return;
        }

        const std::string topic = veda::topic::hwStatus(status.ch);
        const std::string payload = veda::encode(status);
        // retain=true: 클라이언트가 재접속했을 때 마지막 상태를 즉시 받도록
        // (compute-server의 topic::alive(ch) LWT와 동일한 패턴)
        if (publish(topic, payload, veda::qos::kHwStatus, /*retain=*/true)) {
            logSuccess(kIface, "ChannelStatus 발행 성공 topic=" + topic + " (" + label + ")");
        } else {
            // publish() 내부의 실제 mosquitto_publish() 실패는 publish()가 자체적으로 로그를 남김
            // 여기서는 그 앞단(연결 상태 재확인 등)에서 조용히 실패한 경우까지 잡기 위한 안전망
            logError(kIface, "ChannelStatus 발행 실패 topic=" + topic + " (" + label + ")");
        }
    } catch (const std::exception& error) {
        logError(kIface, std::string("ChannelStatus 발행 실패: ") + error.what());
    } catch (...) {
        logError(kIface, "ChannelStatus 발행 실패: 알 수 없는 예외");
    }
}

bool MqttTransport::initializeClient() noexcept {
    if (host_.empty() || port_ <= 0 || port_ > 65535 || keepAliveSeconds_ <= 0 || clientId_.empty()) {
        logError(kIface, "잘못된 설정값");
        return false;
    }
    if (useTls_ && caFile_.empty()) {
        logError(kIface, "TLS 사용 시 CA 파일이 필요함");
        return false;
    }
    if (!clientCertificateFile_.empty() != !clientKeyFile_.empty()) {
        logError(kIface, "클라이언트 인증서와 키는 함께 설정해야 함");
        return false;
    }

    if (!acquireMosquittoLibrary()) {
        logError(kIface, "mosquitto 라이브러리 초기화 실패");
        return false;
    }
    libraryAcquired_ = true;

    client_ = mosquitto_new(clientId_.c_str(), true, this);
    if (client_ == nullptr) {
        logError(kIface, "mosquitto client 생성 실패");
        destroyClient();
        return false;
    }

    mosquitto_connect_callback_set(client_, &MqttTransport::onConnect);
    mosquitto_disconnect_callback_set(client_, &MqttTransport::onDisconnect);
    mosquitto_message_callback_set(client_, &MqttTransport::onMessage);
    mosquitto_reconnect_delay_set(client_, std::max(1, reconnectDelaySeconds_),
                                  std::max(reconnectDelaySeconds_, reconnectDelayMaxSeconds_), true);

    if (!username_.empty()) {
        const int result = mosquitto_username_pw_set(client_, username_.c_str(), password_.c_str());
        if (result != MOSQ_ERR_SUCCESS) {
            logError(kIface, std::string("username/password 설정 실패: ") + mosquitto_strerror(result));
            destroyClient();
            return false;
        }
    }

    if (useTls_) {
        const char* certificate = clientCertificateFile_.empty() ? nullptr : clientCertificateFile_.c_str();
        const char* key = clientKeyFile_.empty() ? nullptr : clientKeyFile_.c_str();
        const int tlsResult = mosquitto_tls_set(client_, caFile_.c_str(), nullptr, certificate, key, nullptr);
        if (tlsResult != MOSQ_ERR_SUCCESS) {
            logError(kIface, std::string("TLS 설정 실패: ") + mosquitto_strerror(tlsResult));
            destroyClient();
            return false;
        }
        mosquitto_tls_insecure_set(client_, tlsInsecure_);
    }

    return true;
}

void MqttTransport::destroyClient() noexcept {
    {
        std::lock_guard lock(clientMutex_);
        if (client_ != nullptr) {
            mosquitto_destroy(client_);
            client_ = nullptr;
        }
    }
    connected_.store(false, std::memory_order_release);
    if (libraryAcquired_) {
        releaseMosquittoLibrary();
        libraryAcquired_ = false;
    }
}

void MqttTransport::notifyConnection(bool connected, int resultCode) noexcept {
    ConnectionHandler handler;
    {
        std::lock_guard lock(callbackMutex_);
        handler = connectionHandler_;
    }
    if (handler) {
        try {
            handler(connected, resultCode);
        } catch (...) {
            logError(kIface, "connection 콜백에서 예외 발생");
        }
    }
}

void MqttTransport::onConnect(mosquitto* client, void* userData, int resultCode) {
    auto* transport = static_cast<MqttTransport*>(userData);
    if (transport == nullptr || !transport->isRunning()) {
        return;
    }

    const bool connected = resultCode == 0;
    transport->connected_.store(connected, std::memory_order_release);
    if (connected) {
        std::vector<Subscription> subscriptions;
        {
            std::lock_guard lock(transport->subscriptionMutex_);
            subscriptions = transport->subscriptions_;
        }
        for (const Subscription& subscription : subscriptions) {
            const int result = mosquitto_subscribe(client, nullptr, subscription.topic.c_str(), subscription.qos);
            if (result != MOSQ_ERR_SUCCESS) {
                logError(kIface,
                         "재연결 후 구독 실패: topic=" + subscription.topic + " error=" + mosquitto_strerror(result));
            }
        }
        logSuccess(kIface, "연결 성공 (host=" + transport->host_ + ", port=" + std::to_string(transport->port_) + ")");
    } else {
        logError(kIface, std::string("연결 거부: ") + mosquitto_connack_string(resultCode));
    }
    transport->notifyConnection(connected, resultCode);
}

void MqttTransport::onDisconnect(mosquitto*, void* userData, int resultCode) {
    auto* transport = static_cast<MqttTransport*>(userData);
    if (transport == nullptr) {
        return;
    }
    transport->connected_.store(false, std::memory_order_release);
    if (transport->isRunning() && resultCode != 0) {
        logError(kIface, std::string("예기치 않은 연결 끊김: ") + mosquitto_strerror(resultCode));
    }
    transport->notifyConnection(false, resultCode);
}

void MqttTransport::onMessage(mosquitto*, void* userData, const mosquitto_message* message) {
    auto* transport = static_cast<MqttTransport*>(userData);
    if (transport == nullptr || message == nullptr || message->topic == nullptr || message->payloadlen < 0) {
        return;
    }

    MessageHandler handler;
    {
        std::lock_guard lock(transport->callbackMutex_);
        handler = transport->messageHandler_;
    }
    if (!handler) {
        return;
    }

    const auto* payload = static_cast<const char*>(message->payload);
    const std::string_view payloadView(payload != nullptr ? payload : "",
                                       static_cast<std::size_t>(message->payloadlen));
    try {
        handler(message->topic, payloadView);
    } catch (...) {
        logError(kIface, "message 콜백에서 예외 발생: topic=" + std::string(message->topic));
    }
}
