#pragma once

/**
 * @file    MqttTopViewSink.h
 * @brief   TopViewFrame 전용 MQTT 발행 Sink
 *
 * @details
 * send()는 큐에 넣고 즉시 반환(논블로킹), 실제 발행은 백그라운드 워커 스레드가 전담
 * 연결 전이거나 큐가 가득 찼을 때는 drop-oldest 정책으로 최신 프레임을 우선함
 * 접속 정보/큐 크기 등은 전부 AppConfig에서 직접 읽어옴 -> 하드코딩도, 별도 Config 복제도 없음
 *
 * @note [ 최초 연결 실패 시 재시도 ]
 * mosquitto의 자동 재접속(mosquitto_reconnect_delay_set)은 "한 번 연결된 뒤 끊긴" 경우만
 * 커버함. 생성자의 최초 initialize()가 실패하면(TLS/CA 파일 오류, 브로커 미기동 등) 워커
 * 스레드가 아예 생성되지 않아 영구히 죽은 상태가 되므로, 실패 시 별도 스레드에서
 * mqttRetryIntervalMs 간격으로 initialize()를 재시도함
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "Contract.h"
#include "core/AppConfig.h"
#include "interfaces/ISink.h"

struct mosquitto;

class MqttTopViewSink final : public ISink<veda::TopViewFrame> {
public:
    explicit MqttTopViewSink(const AppConfig& config);
    ~MqttTopViewSink() override;

    MqttTopViewSink(const MqttTopViewSink&) = delete;
    MqttTopViewSink& operator=(const MqttTopViewSink&) = delete;

    void send(const veda::TopViewFrame& frame) noexcept override;

    bool isReady() const noexcept;
    bool isConnected() const noexcept;

    std::uint64_t publishedCount() const noexcept;
    std::uint64_t droppedCount() const noexcept;

private:
    bool initialize() noexcept;
    void shutdown() noexcept;
    void retryLoop() noexcept;

    void workerLoop() noexcept;
    void publishFrame(const veda::TopViewFrame& frame) noexcept;
    void recordDrop(const char* reason) noexcept;

    bool isValidFrame(const veda::TopViewFrame& frame) const noexcept;

    static void onConnect(struct mosquitto* client, void* userData, int resultCode);
    static void onDisconnect(struct mosquitto* client, void* userData, int resultCode);

    std::string host_;          ///< AppConfig::mqttHost (공통)
    int port_;                  ///< AppConfig::mqttPort (공통)
    std::string caFile_;        ///< AppConfig::mqttCaFile (공통)
    std::string clientId_;      ///< AppConfig::mqttTopViewClientId, 비어있으면 생성자가 자동 생성
    int keepAliveSeconds_;      ///< AppConfig::mqttKeepAliveSeconds (공통)
    std::size_t maxQueueSize_;  ///< AppConfig::mqttTopViewMaxQueueSize (최소 1로 clamp)
    int channelCount_;          ///< AppConfig::channelCount, frame.ch 유효성 검사 범위 [0, channelCount)
    std::chrono::milliseconds retryInterval_;  ///< AppConfig::mqttRetryIntervalMs (공통)

    struct mosquitto* client_ = nullptr;

    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<veda::TopViewFrame> queue_;
    std::thread worker_;

    bool stopping_ = false;
    bool libraryInitialized_ = false;

    std::atomic_bool ready_{false};
    std::atomic_bool connected_{false};
    std::atomic_bool shuttingDown_{false};

    std::atomic_uint64_t publishedCount_{0};
    std::atomic_uint64_t droppedCount_{0};

    std::mutex retryMutex_;
    std::condition_variable retryCv_;
    std::thread retryThread_;
};