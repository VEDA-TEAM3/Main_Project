#pragma once

/**
 * @file    RiskEvaluation.h
 * @brief   프레임 위험 레벨
 */

#include <vector>

#include "Contract.h"

namespace domain {

/**
 * @brief   채널 단위 위험도 요약
 * @details UART로 STM32에 통지하는 이벤트의 페이로드 원본
 *          STM32는 이 값을 받아 LED/siren/buzzer 상태를 자체적으로 결정
 *          관제 서버는 여기서 HW 상태를 직접 제어하지 않음
 */
struct ZoneRisk {
    veda::ChannelId zoneId = -1;                    ///< 대상 채널 (-1 = 미배정)
    veda::RiskLevel level = veda::RiskLevel::None;  ///< 이 zone의 최고 위험 단계
    double minDist = -1.0;                          ///< 이 zone 내 최소 거리(m) (값 없으면 -1)
};

/**
 * @brief 프레임 단위의 최종 위험 판정 결과
 */
struct RiskEvaluation {
    veda::TimestampMs timestamp = 0;
    std::vector<ZoneRisk> zoneLevels;  ///< 채널별 위험도
};

}  // namespace domain