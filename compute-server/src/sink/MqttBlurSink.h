#pragma once

/**
 * @file    MqttBlurSink.h
 * @brief   BlurFrame 전용 MQTT 발행 Sink
 *
 * @details
 * send()는 큐에 넣고 즉시 반환(논블로킹), 실제 발행은 백그라운드 워커 스레드가 전담
 * 연결 전이거나 큐가 가득 찼을 때는 drop-oldest 정책으로 최신 프레임을 우선함
 * 접속 정보/큐 크기 등은 전부 Config(=AppConfig에서 채워짐)를 통해 주입받음 -> 하드코딩 없음
 */

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

class MqttBlurSink final : public ISink<veda::BlurFrame> {
public:
    struct Config {
        std::string host;  ///< MQTT 브로커 주소
        int port = 8883;
        std::string caFile;  ///< TLS CA 인증서 경로
        std::string clientId;  ///< 비어있으면 생성자가 자동 생성

        int keepAliveSeconds = 30;
        std::size_t maxQueueSize = 8;

        /// @brief frame.ch 유효성 검사 범위 [0, channelCount) (AppConfig::channelCount)
        int channelCount = 0;
    };

    explicit MqttBlurSink(Config config);
    ~MqttBlurSink() override;

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

    bool isValidFrame(const veda::BlurFrame& frame) const noexcept;

    static void onConnect(struct mosquitto* client, void* userData, int resultCode);
    static void onDisconnect(struct mosquitto* client, void* userData, int resultCode);

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
