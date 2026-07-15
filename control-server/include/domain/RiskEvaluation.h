#pragma once

/**
 * @file RiskEvaluation.h
 */

#include <vector>

#include "Contract.h"

namespace domain {

/**
 * @brief 채널 단위 위험도 요약
 * @details IHwEventDispatcher 가 UART로 STM32에 통지하는 이벤트의 페이로드 원본
 *          STM32 는 이 값을 받아 LED/siren/buzzer 상태를 자체적으로 결정
 *          관제 서버는 여기서 HW 상태를 직접 제어하지 않음
 */
struct ZoneRisk {
    veda::ChannelId zoneId = -1;                    ///< 대상 채널 (-1 = 미배정)
    veda::RiskLevel level = veda::RiskLevel::None;  ///< 이 zone의 최고 위험 단계
    double minDist = -1.0;                          ///< 이 zone 내 최소 거리(m). 값 없으면 -1
};

/**
 * @brief 프레임 단위의 최종 위험 판정 결과
 * @details
 * - zoneLevels 는 IHwEventDispatcher와 클라이언트 대시보드 표시
 *   양쪽 모두 동일하게 사용 → 별도 판정 로직 없음
 * - 채널 개수는 AppConfig::channelCount 를 따름
 * - IHwEventDispatcher 구현체는 이전 프레임과 zoneLevels 를 비교해 변경분만 UART로 전송
 *   이 비교 로직은 구현체 내부 상태이며 이 구조체 자체엔 이력이 없음
 */
struct RiskEvaluation {
    veda::TimestampMs timestamp = 0;
    veda::RiskLevel overallLevel = veda::RiskLevel::None;  ///< 전체 교차로 최고 위험 단계
    std::vector<ZoneRisk> zoneLevels;  ///< 채널별 위험도. IHwEventDispatcher 이벤트 통지의 직접 입력값
};

}  // namespace domain