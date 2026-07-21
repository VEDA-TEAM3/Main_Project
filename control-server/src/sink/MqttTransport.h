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
 */
class MqttTransport final : public ISink {
public:
    struct Config {
        std::string host = "172.20.27.174";
        int port = 8883;
        std::string clientId = "veda-control";
        int keepAliveSeconds = 60;
        int reconnectDelaySeconds = 1;
        int reconnectDelayMaxSeconds = 10;

        bool useTls = true;
        std::string caFile = "/etc/veda/certs/ca.crt";
        std::string clientCertificateFile;
        std::string clientKeyFile;
        bool tlsInsecure = false;

        std::string username;
        std::string password;
    };

    using MessageHandler = std::function<void(std::string_view topic, std::string_view payload)>;
    using ConnectionHandler = std::function<void(bool connected, int resultCode)>;

    explicit MqttTransport(Config config);
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
    const Config& config() const noexcept { return config_; }

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

    Config config_;
    std::string publishTopic_ = veda::topic::kRisk;
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
