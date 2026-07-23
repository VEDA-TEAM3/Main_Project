#pragma once

/**
 * @file    ThresholdRiskPolicy.h
 * @brief   거리 임계값 기반 위험도 판정 정책
 *
 * @details
 * 거리 판정은 차량을 기준으로만 쿼리 (사람-사람 거리는 계산 안 함)
 * 프레임당 객체 수가 10 이하로 생각되어 브루트포스로 구현
 *
 * @note [ 사람 존재만으로도 Warning ]
 * Contract.h 의 RiskLevel 정의는 `Warning = 사람 감지 or 거리 warningDistance 이내` 임
 * 예전 구현은 차량이 있을 때만 평가해서, 주차장에 사람만 있으면 항상 None 이 나왔음
 * (계약과 구현이 어긋난 상태) -> warnOnHumanPresence 로 계약 쪽에 맞추고, 필요하면
 * 설정으로 끌 수 있게 함
 *
 * @todo
 * 성능 이슈 시 별도 공간 분할/정렬 최적화
 */

#include <memory>

#include "core/AppConfig.h"
#include "interfaces/IDistanceMetric.h"
#include "interfaces/IRiskPolicy.h"

class ThresholdRiskPolicy : public IRiskPolicy {
public:
    /**
     * @brief 생성자
     * @param metric       거리 계산기
     * @param risk         거리 임계값 및 사람 존재 정책 (AppConfig::risk)
     * @param channelCount 채널(zone) 개수
     */
    ThresholdRiskPolicy(std::shared_ptr<IDistanceMetric> metric, const RiskConfig& risk, int channelCount);
    ~ThresholdRiskPolicy() override = default;

    domain::RiskEvaluation evaluate(domain::WorldFrame& frame) override;

private:
    std::shared_ptr<IDistanceMetric> metric_;
    double warningDistance_;
    double dangerousDistance_;
    bool warnOnHumanPresence_;
    int channelCount_;

    /// @brief 위험 판정 로그 rate-limit 용 (차량이 가까이 있는 동안 윈도우마다 반복되므로)
    std::uint64_t riskLogCount_ = 0;
};