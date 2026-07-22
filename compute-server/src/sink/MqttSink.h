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
 * @brief BlurFrame 전용 MQTT 송신 Sink
 *
 * AppContext에서 직접 생성하지 않아도
 * publishBlurToMqtt()를 통해 프로세스 단일 인스턴스를 사용한다.
 */
class MqttBlurSink final : public ISink<veda::BlurFrame> {
public:
    struct Config {
        std::string host;
        int port = 0;
        std::string caFile = "/etc/veda/certs/ca.crt";
        std::string clientId;

        int keepAliveSeconds = 30;
        std::size_t maxQueueSize = 8;
    };

    explicit MqttBlurSink(Config config);
    ~MqttBlurSink() noexcept override;

    MqttBlurSink(const MqttBlurSink&) = delete;
    MqttBlurSink& operator=(const MqttBlurSink&) = delete;

    void send(const veda::BlurFrame& frame) noexcept override;

    bool isReady() const noexcept;
    bool isConnected() const noexcept;

    std::uint64_t publishedCount() const noexcept;
    std::uint64_t droppedCount() const noexcept;

private:
    bool initialize() noexcept;
    void shutdown() noexcept;

    void workerLoop() noexcept;
    void publishFrame(const veda::BlurFrame& frame) noexcept;
    void recordDrop(const char* reason) noexcept;

    static bool isValidFrame(
        const veda::BlurFrame& frame
    ) noexcept;

    static void onConnect(
        struct mosquitto* client,
        void* userData,
        int resultCode
    );

    static void onDisconnect(
        struct mosquitto* client,
        void* userData,
        int resultCode
    );

    Config config_;
    struct mosquitto* client_ = nullptr;

    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<veda::BlurFrame> queue_;
    std::thread worker_;

    bool stopping_ = false;
    bool libraryInitialized_ = false;

    std::atomic_bool ready_{false};
    std::atomic_bool connected_{false};
    std::atomic_bool shuttingDown_{false};

    std::atomic_uint64_t publishedCount_{0};
    std::atomic_uint64_t droppedCount_{0};
};
