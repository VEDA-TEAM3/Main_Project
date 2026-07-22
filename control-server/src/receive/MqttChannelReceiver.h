#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "interfaces/IChannelReceiver.h"

class MqttTransport;

/**
 * @brief Receives TopViewFrame and per-channel alive state through MQTT.
 */
class MqttChannelReceiver final : public IChannelReceiver {
public:
    struct Config {
        int channelCount = 4;
        std::string topViewTopic = veda::topic::kTopViewAll;
        std::string aliveTopic = veda::topic::kAliveAll;
        int topViewQos = veda::qos::kTopView;
        int aliveQos = veda::qos::kAlive;
    };

    MqttChannelReceiver();
    explicit MqttChannelReceiver(std::shared_ptr<MqttTransport> transport);
    MqttChannelReceiver(std::shared_ptr<MqttTransport> transport, Config config);
    ~MqttChannelReceiver() override;

    MqttChannelReceiver(const MqttChannelReceiver&) = delete;
    MqttChannelReceiver& operator=(const MqttChannelReceiver&) = delete;

    void setCallback(FrameCallback callback) override;
    void setAliveCallback(AliveCallback callback) override;
    void start() override;
    void stop() override;

    std::uint64_t receivedCount() const noexcept;
    std::uint64_t droppedCount() const noexcept;

private:
    void handleMessage(std::string_view topic, std::string_view payload) noexcept;
    void handleConnection(bool connected) noexcept;
    std::optional<veda::ChannelId> parseChannel(std::string_view topic, std::string_view suffix) const noexcept;
    void recordDrop(std::string_view topic, const char* reason) noexcept;

    std::shared_ptr<MqttTransport> transport_;
    Config config_;
    mutable std::mutex callbackMutex_;
    FrameCallback frameCallback_;
    AliveCallback aliveCallback_;
    std::atomic_bool running_{false};
    std::atomic_uint64_t receivedCount_{0};
    std::atomic_uint64_t droppedCount_{0};
};
