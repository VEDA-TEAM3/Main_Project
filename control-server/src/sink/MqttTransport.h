#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Contract.h"
#include "interfaces/ISink.h"

struct mosquitto;
struct AppConfig;

/**
 * @brief Owns one Mosquitto client and its TLS/network-loop lifecycle.
 *
 * MqttChannelReceiver and MQTT sinks share this object. Application callbacks
 * are invoked on Mosquitto's network thread and therefore must be non-blocking.
 *
 * 접속 정보는 전부 AppConfig에서 직접 읽어옴 (mqttBrokerUrl을 host/port/TLS 여부로 파싱,
 * clientId/keepAlive/재연결 대기시간도 AppConfig 값을 그대로 사용) -> 하드코딩도,
 * 별도 Config 복제도 없음. 로그는 shared/Logger.h의 logSuccess/logError만 사용
 * (호출 스레드를 막지 않는 비동기 배치 기록 -> mosquitto 콜백 스레드에서 안전하게 호출 가능)
 */
class MqttTransport final : public ISink {
public:
    using MessageHandler = std::function<void(std::string_view topic, std::string_view payload)>;
    using ConnectionHandler = std::function<void(bool connected, int resultCode)>;

    explicit MqttTransport(const AppConfig& config);
    ~MqttTransport() noexcept;

    MqttTransport(const MqttTransport&) = delete;
    MqttTransport& operator=(const MqttTransport&) = delete;

    void setMessageHandler(MessageHandler handler);
    void setConnectionHandler(ConnectionHandler handler);

    /** Stores the subscription and applies it again after every reconnect. */
    bool subscribe(std::string topic, int qos) noexcept;

    bool start() noexcept;
    void stop() noexcept;

    bool publish(std::string_view topic, std::string_view payload, int qos, bool retain = false) noexcept;
    void send(const domain::WorldFrame& frame) override;

    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    bool isConnected() const noexcept { return connected_.load(std::memory_order_acquire); }

private:
    struct Subscription {
        std::string topic;
        int qos = 0;
    };

    static void onConnect(mosquitto* client, void* userData, int resultCode);
    static void onDisconnect(mosquitto* client, void* userData, int resultCode);
    static void onMessage(mosquitto* client, void* userData, const struct mosquitto_message* message);

    bool initializeClient() noexcept;
    void destroyClient() noexcept;
    void notifyConnection(bool connected, int resultCode) noexcept;

    std::string host_ = "172.20.27.174";  ///< AppConfig::mqttBrokerUrl 에서 파싱
    int port_ = 8883;                     ///< AppConfig::mqttBrokerUrl 에서 파싱
    std::string clientId_;                ///< AppConfig::mqttClientId, 비어있으면 자동 생성
    int keepAliveSeconds_ = 60;           ///< AppConfig::mqttKeepAliveSeconds
    int reconnectDelaySeconds_ = 1;       ///< AppConfig::mqttReconnectDelaySeconds
    int reconnectDelayMaxSeconds_ = 10;   ///< AppConfig::mqttReconnectDelayMaxSeconds

    bool useTls_ = true;  ///< AppConfig::mqttBrokerUrl 스킴(tcp/mqtt vs ssl/mqtts)에서 파싱
    std::string caFile_ = "/etc/veda/certs/ca.crt";  ///< AppConfig::mqttCaFile
    std::string clientCertificateFile_;
    std::string clientKeyFile_;
    bool tlsInsecure_ = false;

    std::string username_;
    std::string password_;

    std::string publishTopic_;  ///< AppConfig::mqttSendTopic (비어있으면 veda::topic::kRisk)
    mosquitto* client_ = nullptr;

    mutable std::mutex clientMutex_;
    mutable std::mutex callbackMutex_;
    mutable std::mutex subscriptionMutex_;
    MessageHandler messageHandler_;
    ConnectionHandler connectionHandler_;
    std::vector<Subscription> subscriptions_;

    std::atomic_bool running_{false};
    std::atomic_bool connected_{false};
    bool libraryAcquired_ = false;
};
