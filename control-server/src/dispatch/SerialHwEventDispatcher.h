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

    /**
     * @copydoc IHwEventDispatcher::dispatch
     * @details ConsoleDispatcher와 동일하게 이전에 실제로 전송 성공한 값과 비교해 변경분만 보낸다.
     *          IHwEventDispatcher.h의 @note대로, 비교 기준은 "마지막 전송 성공 값"이어야 유실 시
     *          재전송 누락이 안 생긴다 — write()가 실패하면 lastSentLevel_을 갱신하지 않는다.
     */
    void dispatch(const domain::RiskEvaluation& eval) override;

    /**
     * @copydoc IHwEventDispatcher::setStatusCallback
     * @details readerLoop()는 콜백 등록 여부와 무관하게 생성 시점부터 계속 상행 프레임을 수신해
     *          reportedState_를 갱신해왔다. 여기서 콜백을 뒤늦게 등록하면, 등록 이전에 이미 파악된
     *          채널별 상태는 다음 전환(alive/indicators 변화)이 생길 때까지 통지되지 않으므로,
     *          등록 즉시 현재 reportedState_ 스냅샷을 한 번 통지해 그 공백을 없앤다.
     */
    void setStatusCallback(StatusCallback callback) override;

    /**
     * @copydoc IHwEventDispatcher::setFaultCallback
     * @details setStatusCallback과 동일한 이유로, 등록 즉시 현재 파악된 채널별 fault 상태를
     *          스냅샷으로 한 번 통지해 등록 이전 상태 공백을 없앤다.
     */
    void setFaultCallback(FaultCallback callback) override;

private:
    /**
     * @brief   시리얼 포트를 열고 115200 8N1 raw 모드로 설정한다
     * @details 포트를 못 열어도 예외를 던지지 않는다(AppConfig::load와 동일한 원칙).
     *          fd_ == -1로 남기고, dispatch()/readerLoop()가 이를 감지해 조용히 스킵한다.
     * @todo    재연결 로직은 없음 — 지금은 서버 재시작으로만 복구 가능
     */
    void openPort();

    /**
     * @brief   상행 프레임을 계속 읽어 handleUplinkFrame()으로 넘기는 수신 스레드 루프
     * @details STM32 rx_task와 대칭인 상행 프레임 동기화 상태머신.
     *          START_BYTE를 찾을 때까지 앞의 쓰레기 바이트는 건너뛰고, payload(16B)+checksum+END_BYTE가
     *          모두 맞아야 유효한 프레임으로 처리한다. 체크섬/END가 어긋나면 조용히 버리고 재동기화.
     */
    void readerLoop();

    /**
     * @brief   HEARTBEAT 수신 간격을 감시해 타임아웃된 채널을 dead로 판정하는 스레드 루프
     * @details heartbeatIntervalMs_마다 깨어나서, 마지막 HEARTBEAT 이후
     *          missedBeatsForTimeout_ * heartbeatIntervalMs_를 넘긴 채널을 dead로 판정한다.
     * @warning 타임아웃 채널을 락 안에서 모아두고 락을 푼 뒤에 reportAlive()를 호출한다.
     *          reportAlive()가 같은 heartbeatMutex_(비재귀)를 다시 잠그므로 이 순서를 지켜야 한다.
     */
    void watchdogLoop();

    /**
     * @brief 체크섬·필드 검증을 통과한 상행 프레임 한 개를 처리한다
     *        (alive 갱신 / 표시 상태 통지 / 명령-상태 불일치 검증)
     */
    void handleUplinkFrame(const veda_uplink_packet_t& pkt);

    /**
     * @brief   alive 상태를 갱신(하트비트 수신/watchdog timeout)하고, 바뀌었으면 콜백 통지
     * @details 실제로 바뀐 경우에만 콜백을 통지한다. HEARTBEAT를 받을 때마다(alive=true)
     *          lastHeartbeatAt_는 전이 여부와 무관하게 항상 갱신해야 watchdogLoop()가
     *          타임아웃을 정확히 판단할 수 있다.
     */
    void reportAlive(veda::ChannelId ch, bool alive);

    /**
     * @brief   표시 상태(led/siren/buzzer)를 갱신하고, 바뀌었으면 콜백 통지
     * @details 실제로 바뀐 경우에만 콜백을 통지한다.
     */
    void reportIndicators(veda::ChannelId ch, const HwIndicatorState& indicators);

    /**
     * @brief   ACK/HEARTBEAT의 led_red/led_yellow/led_green(그 채널 LED가 실제로 표시 중인 값)을
     *          RiskLevel로 디코드해(led_red -> Danger, led_yellow -> Warning, 그 외 -> None),
     *          이 채널에 마지막으로 보낸 명령(lastSentLevel_)과 비교해 불일치를 감지한다.
     * @details - 아직 명령을 보낸 적 없는 채널은 비교 기준이 없으므로 스킵
     *          - 일치하면 재시도 카운터를 리셋하고 fault 상태였다면 해소 처리
     *          - 불일치면 재시도 카운터를 올리고 mismatchRetryCount_ 이내면 재전송,
     *            소진되면 mismatchEscalateAfterRetries_ 설정에 따라 에스컬레이션
     * @note  veda_uplink_packet_t 에는 risk_level 필드가 없음 -- 하행(veda_risk_event_t)과
     *        달리 STM32는 실제 LED on/off 상태만 올려보낸다
     */
    void checkChannelMismatch(const veda_uplink_packet_t& pkt);

    /**
     * @brief   불일치 재시도: lastSentLevel_에 저장된 값을 그대로 다시 하행 전송
     * @details 저장된 값을 새 타임스탬프로 재전송한다. 원본 dist_mm은 알 수 없으므로
     *          VEDA_DIST_MM_NONE으로 보낸다 -- 재전송의 목적은 "이 채널이 어떤 risk_level을
     *          표시해야 하는지"를 다시 알리는 것이지 원래 프레임의 거리 측정값을 복원하는 게
     *          아니기 때문이다.
     * @note    호출자(checkChannelMismatch)가 이미 sendStateMutex_를 잡고 있는 상태에서 불린다.
     */
    void resendLastCommand(veda::ChannelId ch, veda::RiskLevel level);

    /**
     * @brief 채널을 fault 상태로 올리고, 전이가 일어났을 때만 faultCallback_ 통지
     * @note  호출자가 이미 sendStateMutex_를 잡고 있는 상태에서 불린다.
     */
    void raiseFault(veda::ChannelId ch);

    /**
     * @brief 채널의 fault 상태를 해소하고, 전이가 일어났을 때만 faultCallback_ 통지
     * @note  호출자가 이미 sendStateMutex_를 잡고 있는 상태에서 불린다.
     */
    void clearFault(veda::ChannelId ch);

    std::string devicePath_;
    uint32_t heartbeatIntervalMs_;
    uint32_t missedBeatsForTimeout_;
    uint32_t mismatchRetryCount_;
    bool mismatchEscalateAfterRetries_;

    int fd_ = -1;

    /// @brief lastSentLevel_/mismatchRetryAttempts_/faultState_/faultCallback_ 보호.
    ///        dispatch()(파이프라인 스레드)와 checkChannelMismatch()(readerLoop 스레드) 양쪽에서 접근한다.
    std::mutex sendStateMutex_;
    /// @brief 채널별로 마지막에 '전송에 성공한' 명령 레벨. 변경분 판정과 불일치 대조의 기준값
    std::unordered_map<veda::ChannelId, veda::RiskLevel> lastSentLevel_;
    /// @brief 채널별 연속 불일치 재시도 횟수. 새 명령 전송 시 리셋되고, mismatchRetryCount_ 초과 시 에스컬레이션
    std::unordered_map<veda::ChannelId, uint32_t> mismatchRetryAttempts_;
    /// @brief 채널별 fault 여부. 전이가 일어난 경우에만 faultCallback_을 부르기 위한 dedup 상태
    std::unordered_map<veda::ChannelId, bool> faultState_;
    FaultCallback faultCallback_;
    StatusCallback statusCallback_;

    /// @brief 채널별로 마지막에 콜백으로 통지한 상태 (dedup 및 setStatusCallback 재등록 시 재생용)
    struct ReportedState {
        bool alive = false;
        HwIndicatorState indicators;
    };

    /// @brief lastHeartbeatAt_/reportedState_/statusCallback_ 보호
    std::mutex heartbeatMutex_;
    /// @brief 채널별 마지막 HEARTBEAT 수신 시각. watchdogLoop()의 타임아웃 판정 기준
    std::unordered_map<veda::ChannelId, std::chrono::steady_clock::time_point> lastHeartbeatAt_;
    std::unordered_map<veda::ChannelId, ReportedState> reportedState_;

    std::atomic<bool> running_{false};
    std::thread readerThread_;
    std::thread watchdogThread_;
};
