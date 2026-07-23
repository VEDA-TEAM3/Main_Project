#pragma once

/**
 * @file    MqttTransport.h
 * @brief   mosquitto 클라이언트 하나의 생명주기를 소유하는 MQTT 전송 계층
 *
 * @details
 * 접속 정보는 전부 AppConfig 에서 읽어옴 -> 하드코딩도, 별도 Config 복제도 없음
 * 최초 접속 실패 시 mqttRetryIntervalMs 간격으로 백그라운드 재시도함
 * (mosquitto 의 자동 재접속은 '한 번 연결된 뒤 끊긴' 경우만 커버하기 때문)
 *
 * @note [ 라이브러리 참조 카운트 ]
 * mosquitto_lib_init()/cleanup() 은 전역 상태를 다루므로, 여러 인스턴스가 생겨도
 * 마지막 하나가 사라질 때만 cleanup 하도록 참조 카운트로 감쌌음
 * (control-server 의 MqttTransport 와 동일한 방식)
 *
 * @note [ 채널 생존 신호 (LWT) ]
 * veda::topic::alive(ch) 에 retained 로 "1"(살아있음)/"0"(죽음)을 실어보냄
 * - 접속 전에 mosquitto_will_set() 으로 "0" 을 유언으로 걸어둠
 *   -> 프로세스가 죽거나 네트워크가 끊기면 브로커가 대신 "0" 을 발행
 * - 접속에 성공할 때마다 "1" 을 직접 발행 (재접속 시에도 되살아났음을 알림)
 * - 정상 종료 시에는 유언을 기다리지 않고 "0" 을 먼저 발행
 *
 * 이 신호가 있어야 control-server 가 '객체가 없는 빈 프레임'과 '채널이 죽어서
 * 아무것도 안 오는 상태'를 구분할 수 있음 (Contract.h 가 정의하고
 * MqttChannelReceiver 가 이미 구독 중인데 발행하는 쪽이 없었음)
 *
 * 연결 자체를 소유한 이 클래스가 담당함 -- 유언은 접속 이전에 걸어야 하고
 * 되살아남 통지는 (재)접속 콜백에서 해야 하므로 Sink 쪽에서는 할 수 없는 일
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/IMqttTransport.h"

struct mosquitto;

class MqttTransport final : public IMqttTransport {
public:
    explicit MqttTransport(const AppConfig& config);
    ~MqttTransport() override;

    MqttTransport(const MqttTransport&) = delete;
    MqttTransport& operator=(const MqttTransport&) = delete;

    ListenerId addConnectionListener(std::function<void(bool)> listener) override;
    void removeConnectionListener(ListenerId id) noexcept override;

    bool start() noexcept override;
    void stop() noexcept override;

    bool publish(std::string_view topic, std::string_view payload, int qos, bool retain = false) noexcept override;

    bool isReady() const noexcept override { return ready_.load(std::memory_order_acquire); }
    bool isConnected() const noexcept override { return connected_.load(std::memory_order_acquire); }

private:
    bool initializeClient() noexcept;
    void destroyClient() noexcept;
    void retryLoop() noexcept;
    void notifyListeners(bool connected) noexcept;

    static void onConnect(mosquitto* client, void* userData, int resultCode);
    static void onDisconnect(mosquitto* client, void* userData, int resultCode);

    std::string host_;                         ///< AppConfig::mqttHost
    int port_;                                 ///< AppConfig::mqttPort
    std::string caFile_;                       ///< AppConfig::mqttCaFile
    std::string clientId_;                     ///< AppConfig::mqttClientId, 비어있으면 자동 생성
    int keepAliveSeconds_;                     ///< AppConfig::mqttKeepAliveSeconds
    std::chrono::milliseconds retryInterval_;  ///< AppConfig::mqttRetryIntervalMs
    int reconnectDelaySec_;                    ///< AppConfig::mqttReconnectDelaySec
    int reconnectDelayMaxSec_;                 ///< AppConfig::mqttReconnectDelayMaxSec

    /// @brief veda::topic::alive(channelId) — 채널이 고정이라 생성 시 1회만 계산
    std::string aliveTopic_;

    mosquitto* client_ = nullptr;

    /// @brief client_ 수명 보호 (publish 는 sink 워커 스레드들에서, destroy 는 종료 시점에 일어남)
    mutable std::mutex clientMutex_;

    struct Listener {
        ListenerId id = kInvalidListener;
        std::function<void(bool)> callback;
    };

    /**
     * @brief   리스너 목록 보호
     * @details 콜백 '실행 중'에도 계속 잡고 있음 -> removeConnectionListener() 가 반환하면
     *          해당 콜백이 절대 실행 중이 아님을 보장 (Sink 파괴 시 use-after-free 방지)
     *          콜백은 조건변수 notify 정도만 하므로 잡고 있어도 길지 않음
     */
    mutable std::mutex listenerMutex_;
    std::vector<Listener> connectionListeners_;
    ListenerId nextListenerId_ = 1;

    std::mutex retryMutex_;
    std::condition_variable retryCv_;
    std::thread retryThread_;

    std::atomic_bool ready_{false};
    std::atomic_bool connected_{false};
    std::atomic_bool stopping_{false};

    bool libraryAcquired_ = false;
};
