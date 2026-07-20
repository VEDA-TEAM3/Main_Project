#pragma once

/**
 * @file    IRiskPolicy.h
 * @brief   프레임 내 모든 객체의 위험 레벨을 평가하는 인터페이스
 */

#include "domain/RiskEvaluation.h"
#include "domain/WorldFrame.h"

class IRiskPolicy {
public:
    virtual ~IRiskPolicy() = default;

    /**
     * @brief   프레임 내 모든 객체의 위험도를 평가
     * @param   frame 평가 대상이 되는 현재 교차로 전체 프레임
     *
     * @pre     각 객체의 WorldObject::zoneId가 IZoneMapper에 의해 이미 배정되어 있어야 함
     *
     * @return  프레임의 위험 상태
     *
     * @note    frame은 이 호출 후 각 객체의 riskLevel/nearestObj/nearestDist가 채워짐
     */
    virtual domain::RiskEvaluation evaluate(domain::WorldFrame& frame) = 0;
};