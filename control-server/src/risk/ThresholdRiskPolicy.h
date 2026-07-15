#pragma once

/**
 * @file ThresholdRiskPolicy.h
 * @brief 거리 임계값 기반 위험도 판정 정책
 * @details
 * x좌표 기반 정렬을 통해 최적화된 최단 거리 탐색을 수행하고,
 * 설정된 경고 및 위험 반경에 따라 프레임 전체의 위험도를 평가
 */

#include <memory>

#include "interfaces/IDistanceMetric.h"
#include "interfaces/IRiskPolicy.h"

class ThresholdRiskPolicy : public IRiskPolicy {
public:
    /**
     * @brief 생성자
     * @param metric 거리 계산기
     * @param warningDist 경고 판정 거리 (단위: m)
     * @param dangerDist 위험 판정 거리 (단위: m)
     */
    ThresholdRiskPolicy(std::shared_ptr<IDistanceMetric> metric, double warningDist, double dangerDist);
    ~ThresholdRiskPolicy() override = default;

    domain::RiskEvaluation evaluate(domain::WorldFrame& frame) override;

private:
    std::shared_ptr<IDistanceMetric> metric_;
    double warningDist_;
    double dangerDist_;
};