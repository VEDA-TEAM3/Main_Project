#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "Contract.h"
#include "interfaces/ISink.h"

struct mosquitto;

/**
 * @brief TopViewFrame을 계약 토픽으로 비동기 발행하고 채널 LWT를 관리한다.
 */
class MqttTopViewSink final : public ISink<veda::TopViewFrame> {
public:
    struct Config {
        std::string host = "100.73.128.114";
        int port = 8883;
        std::string caCertificatePath = "/etc/veda/certs/ca.crt";
        std::string clientId;
        int keepAliveSeconds = 60;
        size_t maxQueueSize = 8;
    };

    MqttTopViewSink(veda::ChannelId channelId, Config config);
    ~MqttTopViewSink() noexcept override;

    MqttTopViewSink(const MqttTopViewSink&) = delete;
    MqttTopViewSink& operator=(const MqttTopViewSink&) = delete;

    void send(const veda::TopViewFrame& frame) override;

    bool isReady() const noexcept { return ready_.load(std::memory_order_acquire); }
    bool isConnected() const noexcept { return connected_.load(std::memory_order_acquire); }

private:
    bool initialize() noexcept;
    void shutdown() noexcept;
    void workerLoop() noexcept;
    void publishFrame(const veda::TopViewFrame& frame) noexcept;
    void publishAlive(bool alive) noexcept;
    void recordDrop(const char* reason) noexcept;

    static void onConnect(struct mosquitto* client, void* userData, int resultCode);
    static void onDisconnect(struct mosquitto* client, void* userData, int resultCode);

    veda::ChannelId channelId_ = 0;
    Config config_;
    struct mosquitto* client_ = nullptr;

    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<veda::TopViewFrame> queue_;
    std::thread worker_;
    bool stopping_ = false;

    std::atomic_bool ready_{false};
    std::atomic_bool connected_{false};
    std::atomic_bool shuttingDown_{false};
    std::atomic_uint64_t droppedCount_{0};
};

void publishTopViewToMqtt(
    const veda::TopViewFrame& frame
) noexcept;