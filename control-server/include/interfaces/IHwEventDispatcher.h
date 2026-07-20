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

class IHwEventDispatcher {
public:
    virtual ~IHwEventDispatcher() = default;

    /**
     * @brief STM32 하트비트(상태 보고) 수신 시 호출될 콜백 함수 타입
     * @param ch 보고를 보낸 채널
     * @param alive 정상 응답 여부 (false = missedBeatsForTimeout초과로 dead 판정)
     */
    using StatusCallback = std::function<void(veda::ChannelId ch, bool alive)>;

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
};