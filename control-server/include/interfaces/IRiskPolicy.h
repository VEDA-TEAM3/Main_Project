#pragma once

#include "domain/RiskEvaluation.h"
#include "domain/WorldFrame.h"

class IRiskPolicy {
public:
    virtual ~IRiskPolicy() = default;

    /**
     * @brief 프레임 내 모든 객체의 위험도를 평가
     * @param frame 평가 대상이 되는 현재 교차로 전체 프레임
     * @pre 각 객체의 WorldObject::zoneId 가 IZoneMapper 에 의해 이미 배정되어 있어야 함
     * @return domain::RiskEvaluation zoneId 기준으로 grouping된 zoneLevels 포함, 전체 프레임의 최종 위험 상태
     * @note frame 은 이 호출 후 각 객체의 riskLevel/nearestObj/nearestDist 가 in-place 로 채워짐
     */
    virtual domain::RiskEvaluation evaluate(domain::WorldFrame& frame) = 0;
};