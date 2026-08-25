#pragma once

/**
 * @file    IHwEventDispatcher.h
 * @brief   STM32로 위험 이벤트를 통지하고, STM32의 HW 상태 보고를 수신하는 양방향 인터페이스
 *
 * @details
 * 관제 서버는 LED/siren/buzzer 상태를 직접 결정하지 않음
 * 채널별 위험 이벤트를 UART로 통지할 뿐이며,
 * 실제 HW 제어는 STM32가 로컬에서 판단해 수행
 *
 * 하나의 물리 UART 링크를 통해 하행(이벤트 통지)과 상행(하트비트 수신)이 함께
 * 이뤄지므로 두 책임을 하나의 인터페이스로 묶음
 */

#include <functional>

#include "Contract.h"
#include "domain/RiskEvaluation.h"

/**
 * @brief   STM32가 상행(veda_uplink_packet_t, driver_protocol.h)으로 보고하는 실제 표시 상태
 *
 * @warning [ STRICT 계약 — alive 와 반드시 함께 해석할 것 ]
 * 이 필드들은 alive 로 게이트되어야 하는 값이다:
 *  - alive=true  : 실시간 유효값
 *  - alive=false : watchdog timeout 직전 '마지막으로 확인된' 값(=stale) -- 최신값이 아니다
 * StatusCallback 수신 측(Controller)은 이 값을 그대로 보존해 veda::ChannelStatus 로 전달하고,
 * 최종 소비자(Qt)까지 동일한 계약이 이어진다. 어느 계층도 alive 를 무시한 채 표시 상태만
 * 활성으로 해석해서는 안 된다. (하류 계약: Contract.h::ChannelStatus 의 클라이언트 계약)
 */
struct HwIndicatorState {
    bool sirenOn = false;
    bool buzzerOn = false;
    bool ledRed = false;
    bool ledYellow = false;
    bool ledGreen = false;

    bool operator==(const HwIndicatorState&) const = default;
};

class IHwEventDispatcher {
public:
    virtual ~IHwEventDispatcher() = default;

    /**
     * @brief STM32 상태 보고(ACK/HEARTBEAT) 수신 시 호출될 콜백 함수 타입
     * @param ch          보고를 보낸 채널
     * @param alive       정상 응답 여부 (false = missedBeatsForTimeout초과로 dead 판정)
     * @param indicators  그 채널이 실제로 켜고 있는 경광등/부저/LED 상태
     *
     * @note alive 와 indicators 중 하나라도 바뀌면 호출됨 (변화 없으면 호출 안 됨).
     *       alive=false 로 호출될 때의 indicators 는 끊기기 직전 마지막으로 확인된 값이다.
     */
    using StatusCallback = std::function<void(veda::ChannelId ch, bool alive, const HwIndicatorState& indicators)>;

    /**
     * @brief 명령-실제상태 불일치가 재시도 소진 후에도 해소되지 않을 때 호출될 콜백 함수 타입
     * @param ch 대상 채널
     * @param faulted true = 재시도 소진 후 에스컬레이션(불일치 지속), false = 해소되어 정상 복귀
     */
    using FaultCallback = std::function<void(veda::ChannelId ch, bool faulted)>;

    /**
     * @brief 위험 평가 결과를 UART 이벤트로 통지 (하행)
     * @param eval 현재 프레임의 zoneLevels
     *
     * @note
     * - 구현체는 이전에 실제로 전송에 성공한 값과 비교해 변경분만 전송
     * 전송 실패(유실) 시 다음 프레임에서 값이 안 바뀌었다는 이유로 재전송을
     * 누락하면 안 되므로, 비교 기준은 반드시 마지막 전송 성공 값 이어야 함
     */
    virtual void dispatch(const domain::RiskEvaluation& eval) = 0;

    /**
     * @brief STM32 상태 보고 콜백 등록 (상행)
     * @param callback 하트비트 수신 시 실행할 함수
     */
    virtual void setStatusCallback(StatusCallback callback) = 0;

    /**
     * @brief 명령-실제상태 불일치 에스컬레이션 콜백 등록 (상행)
     * @param callback 재시도 소진/해소 시 실행할 함수
     */
    virtual void setFaultCallback(FaultCallback callback) = 0;
};