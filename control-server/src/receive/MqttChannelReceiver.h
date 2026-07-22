#pragma once

/**
 * @file    MqttChannelReceiver.h
 * @brief   MqttTransport를 통해 TopViewFrame과 채널별 alive 상태를 수신
 *
 * @note [ Transport 공유 ]
 * MqttTransport는 이 receiver와 MQTT sink(발행 경로)가 함께 쓰는 하나의 mosquitto
 * 클라이언트/연결임 (MqttTransport.h 참고). 반드시 AppContext가 만든 동일 인스턴스를
 * 주입받아야 하며, 여기서 새 인스턴스를 만들면 커넥션(TLS 포함)이 두 개로 늘어나
 * 지연시간과 리소스를 낭비하게 됨 -> 그래서 stop()도 transport 자체는 건드리지 않고
 * 핸들러 등록만 해제함 (transport 수명주기는 공유 소유자인 AppContext/Controller 몫)
 *
 * @note [ 최초 연결 실패 시 재시도 ]
 * IChannelReceiver.h의 계약대로 연결 실패는 예외를 던지지 않고 구현체 내부에서 재시도함.
 * start() 시점에 transport 구독/시작이 바로 성공하면 재시도 스레드는 만들지 않고,
 * 실패했을 때만 백그라운드 스레드를 띄워 mqttReceiverRetryIntervalMs 간격으로 재시도함
 * (mosquitto의 자동 재접속은 "한 번 연결된 뒤 끊긴" 경우만 커버하므로, 최초 연결 자체가
 * 실패하는 경우는 별도로 재시도해야 함)
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "interfaces/IChannelReceiver.h"

class MqttTransport;

class MqttChannelReceiver final : public IChannelReceiver {
public:
    /**
     * @param transport         sink와 공유하는 MQTT 연결 (null이면 start()가 실패하고 로그만 남김)
     * @param channelCount      frame.ch / topic 채널 번호의 유효 범위 [0, channelCount) (AppConfig::channelCount)
     * @param retryIntervalMs   최초 연결 실패 시 재시도 간격 (AppConfig::mqttReceiverRetryIntervalMs)
     */
    MqttChannelReceiver(std::shared_ptr<MqttTransport> transport, int channelCount, std::uint64_t retryIntervalMs);
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
    bool tryConnect() noexcept;
    void retryLoop() noexcept;

    void handleMessage(std::string_view topic, std::string_view payload) noexcept;
    void handleConnection(bool connected) noexcept;
    std::optional<veda::ChannelId> parseChannel(std::string_view topic, std::string_view suffix) const noexcept;
    void recordDrop(std::string_view topic, const char* reason) noexcept;

    std::shared_ptr<MqttTransport> transport_;
    int channelCount_;
    std::chrono::milliseconds retryInterval_;

    mutable std::mutex callbackMutex_;
    FrameCallback frameCallback_;
    AliveCallback aliveCallback_;
    std::atomic_bool running_{false};
    std::atomic_uint64_t receivedCount_{0};
    std::atomic_uint64_t droppedCount_{0};

    std::mutex retryMutex_;
    std::condition_variable retryCv_;
    std::thread retryThread_;
};
