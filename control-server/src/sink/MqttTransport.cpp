#include "sink/MqttTransport.h"

#include <mosquitto.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <utility>

#include "core/AppConfig.h"

namespace {

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

int riskRank(veda::RiskLevel level) noexcept {
    switch (level) {
        case veda::RiskLevel::Danger:
            return 2;
        case veda::RiskLevel::Warning:
            return 1;
        case veda::RiskLevel::None:
            return 0;
    }
    return 0;
}

MqttTransport::Config transportConfigFrom(const AppConfig& appConfig) {
    MqttTransport::Config config;
    config.host = appConfig.mqttHost;
    config.port = appConfig.mqttPort;
    config.useTls = appConfig.mqttUseTls;
    config.caFile = appConfig.mqttCaFile;
    config.clientId = appConfig.mqttClientId;
    config.keepAliveSeconds = appConfig.mqttKeepAliveSeconds;
    return config;
}

veda::RiskFrame toRiskFrame(const domain::WorldFrame& frame) {
    veda::RiskFrame result;
    result.ts = frame.timestamp;
    result.objects.reserve(frame.objects.size());

    for (const domain::WorldObject& source : frame.objects) {
        veda::RiskObject object;
        object.gid = source.gid;
        object.cls = source.cls;
        object.pos = veda::WorldPoint{source.pos.x, source.pos.y};
        object.level = source.riskLevel;
        object.nearest = source.nearestObj;
        object.dist = source.nearestDist;
        result.objects.push_back(object);

        if (riskRank(source.riskLevel) > riskRank(result.level)) {
            result.level = source.riskLevel;
        }
    }
    return result;
}

}  // namespace

MqttTransport::MqttTransport(Config config) : config_(std::move(config)) {}

MqttTransport::MqttTransport(const AppConfig& config) : MqttTransport(transportConfigFrom(config)) {
    publishTopic_ = config.mqttSendTopic.empty() ? std::string(veda::topic::kRisk) : config.mqttSendTopic;
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
        std::fprintf(stderr, "[MqttTransport] subscribe failed: topic=%s error=%s\n", subscriptionTopic.c_str(),
                     mosquitto_strerror(result));
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

    const int connectResult =
        mosquitto_connect_async(client_, config_.host.c_str(), config_.port, config_.keepAliveSeconds);
    if (connectResult != MOSQ_ERR_SUCCESS) {
        std::fprintf(stderr, "[MqttTransport] connect setup failed: host=%s port=%d error=%s\n", config_.host.c_str(),
                     config_.port, mosquitto_strerror(connectResult));
        running_.store(false, std::memory_order_release);
        destroyClient();
        return false;
    }

    const int loopResult = mosquitto_loop_start(client_);
    if (loopResult != MOSQ_ERR_SUCCESS) {
        std::fprintf(stderr, "[MqttTransport] network loop start failed: %s\n", mosquitto_strerror(loopResult));
        running_.store(false, std::memory_order_release);
        destroyClient();
        return false;
    }

    std::fprintf(stdout, "[MqttTransport] connecting: host=%s port=%d tls=%s clientId=%s\n", config_.host.c_str(),
                 config_.port, config_.useTls ? "on" : "off", config_.clientId.c_str());
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
        std::fprintf(stderr, "[MqttTransport] publish failed: topic=%s error=%s\n", topicString.c_str(),
                     mosquitto_strerror(result));
        return false;
    }
    return true;
}

void MqttTransport::send(const domain::WorldFrame& frame) {
    try {
        if (frame.timestamp <= 0) {
            std::fprintf(stderr, "[MqttTransport] invalid WorldFrame timestamp\n");
            return;
        }
        if (!isRunning() && !start()) {
            return;
        }
        if (!isConnected()) {
            return;
        }

        const std::string payload = veda::encode(toRiskFrame(frame));
        publish(publishTopic_, payload, veda::qos::kRisk, false);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[MqttTransport] WorldFrame publish failed: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "[MqttTransport] WorldFrame publish failed: unknown exception\n");
    }
}

bool MqttTransport::initializeClient() noexcept {
    if (config_.host.empty() || config_.port <= 0 || config_.port > 65535 || config_.keepAliveSeconds <= 0 ||
        config_.clientId.empty()) {
        std::fprintf(stderr, "[MqttTransport] invalid configuration\n");
        return false;
    }
    if (config_.useTls && config_.caFile.empty()) {
        std::fprintf(stderr, "[MqttTransport] TLS requires a CA file\n");
        return false;
    }
    if (!config_.clientCertificateFile.empty() != !config_.clientKeyFile.empty()) {
        std::fprintf(stderr, "[MqttTransport] client certificate and key must be configured together\n");
        return false;
    }

    if (!acquireMosquittoLibrary()) {
        std::fprintf(stderr, "[MqttTransport] mosquitto library initialization failed\n");
        return false;
    }
    libraryAcquired_ = true;

    client_ = mosquitto_new(config_.clientId.c_str(), true, this);
    if (client_ == nullptr) {
        std::fprintf(stderr, "[MqttTransport] client allocation failed\n");
        destroyClient();
        return false;
    }

    mosquitto_connect_callback_set(client_, &MqttTransport::onConnect);
    mosquitto_disconnect_callback_set(client_, &MqttTransport::onDisconnect);
    mosquitto_message_callback_set(client_, &MqttTransport::onMessage);
    mosquitto_reconnect_delay_set(client_, std::max(1, config_.reconnectDelaySeconds),
                                  std::max(config_.reconnectDelaySeconds, config_.reconnectDelayMaxSeconds), true);

    if (!config_.username.empty()) {
        const int result = mosquitto_username_pw_set(client_, config_.username.c_str(), config_.password.c_str());
        if (result != MOSQ_ERR_SUCCESS) {
            std::fprintf(stderr, "[MqttTransport] username/password setup failed: %s\n", mosquitto_strerror(result));
            destroyClient();
            return false;
        }
    }

    if (config_.useTls) {
        const char* certificate =
            config_.clientCertificateFile.empty() ? nullptr : config_.clientCertificateFile.c_str();
        const char* key = config_.clientKeyFile.empty() ? nullptr : config_.clientKeyFile.c_str();
        const int tlsResult = mosquitto_tls_set(client_, config_.caFile.c_str(), nullptr, certificate, key, nullptr);
        if (tlsResult != MOSQ_ERR_SUCCESS) {
            std::fprintf(stderr, "[MqttTransport] TLS setup failed: %s\n", mosquitto_strerror(tlsResult));
            destroyClient();
            return false;
        }
        mosquitto_tls_insecure_set(client_, config_.tlsInsecure);
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
            std::fprintf(stderr, "[MqttTransport] connection callback threw an exception\n");
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
                std::fprintf(stderr, "[MqttTransport] reconnect subscribe failed: topic=%s error=%s\n",
                             subscription.topic.c_str(), mosquitto_strerror(result));
            }
        }
        std::fprintf(stdout, "[MqttTransport] connected: host=%s port=%d\n", transport->config_.host.c_str(),
                     transport->config_.port);
    } else {
        std::fprintf(stderr, "[MqttTransport] connection rejected: %s\n", mosquitto_connack_string(resultCode));
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
        std::fprintf(stderr, "[MqttTransport] disconnected unexpectedly: %s\n", mosquitto_strerror(resultCode));
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
        std::fprintf(stderr, "[MqttTransport] message callback threw an exception: topic=%s\n", message->topic);
    }
}
