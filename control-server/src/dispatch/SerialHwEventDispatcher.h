#pragma once

/**
 * @file    SerialHwEventDispatcher.h
 * @brief   STM32로 위험 이벤트를 실제 UART로 통지하고, STM32의 ACK/HEARTBEAT를 수신하는 구현체
 * @details ConsoleDispatcher의 "변경분만 전송" 로직은 그대로 가져오되, 콘솔 출력 대신
 *          driver_protocol.h(veda 바이너리 프로토콜)로 실제 시리얼 포트에 쓴다.
 *          별도 스레드가 상행 프레임(ACK/HEARTBEAT)을 계속 읽고, HEARTBEAT 수신 간격을
 *          감시해 missedBeatsForTimeout을 넘기면 setStatusCallback으로 등록된 콜백에
 *          alive=false를 통지한다. 매 상행 프레임의 siren_on/buzzer_on/led_* 상태도
 *          HwIndicatorState로 담아 alive 와 함께 전달한다(둘 중 하나라도 바뀔 때만 호출).
 */

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "Contract.h"
#include "driver_protocol.h"
#include "interfaces/IHwEventDispatcher.h"

class SerialHwEventDispatcher : public IHwEventDispatcher {
public:
    /**
     * @param devicePath                   시리얼 장치 경로 (예: "/dev/serial0")
     * @param heartbeatIntervalMs          STM32가 HEARTBEAT를 보내기로 되어 있는 주기 (ms)
     * @param missedBeatsForTimeout        연속 몇 번 유실되면 dead 판정할지
     * @param mismatchRetryCount           명령-실제상태 불일치 시 재전송 횟수
     * @param mismatchEscalateAfterRetries 재시도 소진 시 setFaultCallback으로 등록된 콜백에 알릴지 여부
     */
    SerialHwEventDispatcher(std::string devicePath, uint32_t heartbeatIntervalMs, uint32_t missedBeatsForTimeout,
                            uint32_t mismatchRetryCount, bool mismatchEscalateAfterRetries);
    ~SerialHwEventDispatcher() override;

    SerialHwEventDispatcher(const SerialHwEventDispatcher&) = delete;
    SerialHwEventDispatcher& operator=(const SerialHwEventDispatcher&) = delete;

    void dispatch(const domain::RiskEvaluation& eval) override;
    void setStatusCallback(StatusCallback callback) override;
    void setFaultCallback(FaultCallback callback) override;

private:
    void openPort();
    void readerLoop();
    void watchdogLoop();
    void handleUplinkFrame(const veda_uplink_packet_t& pkt);

    /// @brief alive 상태를 갱신(하트비트 수신/watchdog timeout)하고, 바뀌었으면 콜백 통지
    void reportAlive(veda::ChannelId ch, bool alive);
    /// @brief 표시 상태(led/siren/buzzer)를 갱신하고, 바뀌었으면 콜백 통지
    void reportIndicators(veda::ChannelId ch, const HwIndicatorState& indicators);

    /**
     * @brief ACK/HEARTBEAT의 led_red/led_yellow/led_green(그 채널 LED가 실제로 표시 중인 값)을
     *        RiskLevel로 디코드해(led_red -> Danger, led_yellow -> Warning, 그 외 -> None),
     *        이 채널에 마지막으로 보낸 명령(lastSentLevel_)과 비교해 불일치를 감지한다.
     * @note  veda_uplink_packet_t 에는 risk_level 필드가 없음 -- 하행(veda_risk_event_t)과
     *        달리 STM32는 실제 LED on/off 상태만 올려보낸다
     */
    void checkChannelMismatch(const veda_uplink_packet_t& pkt);
    /// @brief 불일치 재시도: lastSentLevel_에 저장된 값을 그대로 다시 하행 전송
    void resendLastCommand(veda::ChannelId ch, veda::RiskLevel level);
    void raiseFault(veda::ChannelId ch);
    void clearFault(veda::ChannelId ch);

    std::string devicePath_;
    uint32_t heartbeatIntervalMs_;
    uint32_t missedBeatsForTimeout_;
    uint32_t mismatchRetryCount_;
    bool mismatchEscalateAfterRetries_;

    int fd_ = -1;

    // lastSentLevel_/mismatchRetryAttempts_/faultState_는 dispatch()(파이프라인 스레드)와
    // checkChannelMismatch()(readerLoop 스레드) 양쪽에서 접근하므로 sendStateMutex_로 보호한다.
    std::mutex sendStateMutex_;
    std::unordered_map<veda::ChannelId, veda::RiskLevel> lastSentLevel_;
    std::unordered_map<veda::ChannelId, uint32_t> mismatchRetryAttempts_;
    std::unordered_map<veda::ChannelId, bool> faultState_;
    FaultCallback faultCallback_;
    StatusCallback statusCallback_;

    /// @brief 채널별로 마지막에 콜백으로 통지한 상태 (dedup 및 setStatusCallback 재등록 시 재생용)
    struct ReportedState {
        bool alive = false;
        HwIndicatorState indicators;
    };

    std::mutex heartbeatMutex_;
    std::unordered_map<veda::ChannelId, std::chrono::steady_clock::time_point> lastHeartbeatAt_;
    std::unordered_map<veda::ChannelId, ReportedState> reportedState_;

    std::atomic<bool> running_{false};
    std::thread readerThread_;
    std::thread watchdogThread_;
};
