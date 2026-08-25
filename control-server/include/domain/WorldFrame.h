#pragma once

/**
 * @file    WorldFrame.h
 * @brief   특정 Timestamp에 채널이 융합된 교차로 전체의 프레임 상태
 */

#include <vector>

#include "Contract.h"
#include "domain/WorldObject.h"

namespace domain {

struct WorldFrame {
    veda::TimestampMs timestamp = 0;   ///< Timestamp (UTC)
    std::vector<WorldObject> objects;  ///< 융합이 완료된 전체 객체 목록

    /**
     * @brief 프레임 전체의 최고 위험 레벨 (UI RiskFrame.level 의 유일한 출처)
     *
     * @note [ 단일 진실 공급원 — UI == HW (Rule 4) ]
     * ThresholdRiskPolicy 가 채널별 위험도(RiskEvaluation::zoneLevels, HW/UART 로 나가는 값)를
     * 구한 뒤 그 max 로 이 필드를 채운다. UI(MqttTransport)는 이 값을 그대로 읽기만 하고
     * 절대 재계산하지 않는다 -> UI 로 나가는 전체 위험도와 HW 로 나가는 채널 위험도가
     * 같은 소스(zoneLevels)에서 파생되어 구조적으로 어긋날 수 없다.
     */
    veda::RiskLevel level = veda::RiskLevel::None;
};

}  // namespace domain