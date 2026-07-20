#pragma once

/**
 * @file    ThresholdRiskPolicy.h
 * @brief   거리 임계값 기반 위험도 판정 정책
 *
 * @details
 * 차량을 기준으로만 쿼리 (사람-사람 거리는 계산 안 함)
 * 프레임당 객체 수가 10 이하로 생각되어 브루트포스로 구현
 *
 * @todo
 * 성능 이슈 시 별도 공간 분할/정렬 최적화
 */

#include <memory>

#include "interfaces/IDistanceMetric.h"
#include "interfaces/IRiskPolicy.h"

class ThresholdRiskPolicy : public IRiskPolicy {
public:
    /**
     * @brief 생성자
     * @param metric 거리 계산기
     * @param warningDistance 경고 판정 거리 (m)
     * @param dangerousDistance 위험 판정 거리 (m)
     * @param channelCount 채널(zone) 개수
     */
    ThresholdRiskPolicy(std::shared_ptr<IDistanceMetric> metric, double warningDistance, double dangerousDistance,
                        int channelCount);
    ~ThresholdRiskPolicy() override = default;

    domain::RiskEvaluation evaluate(domain::WorldFrame& frame) override;

private:
    std::shared_ptr<IDistanceMetric> metric_;
    double warningDistance_;
    double dangerousDistance_;
    int channelCount_;
};