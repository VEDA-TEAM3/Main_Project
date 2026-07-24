#pragma once

/**
 * @file    ThresholdRiskPolicy.h
 * @brief   거리 임계값 기반 위험도 판정 정책
 *
 * @details
 * [ 위험 판정 5원칙 ]
 *  1. 차량이 없으면 위험도 없음 (사람만 있으면 전부 None).
 *  2. 차량이 검출되면 그 차량 기준으로 주변 객체까지의 거리를 잰다.
 *  3. 판정된 위험 레벨을 차량과 그 최근접 객체 '양쪽'에 부여한다 (상호 부여, 올리기만).
 *  4. UI(MQTT)와 HW(UART)로 나가는 위험도는 동일 소스(zoneLevels)에서 파생된다.
 *  5. 채널(zone) 위험도 = 그 채널 안 '차량'들의 위험 레벨 max.
 *
 * 거리 판정은 차량을 기준으로만 쿼리 (사람-사람 거리는 계산 안 함).
 * 프레임당 객체 수가 10 이하로 생각되어 브루트포스로 구현.
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
    int channelCount_;

    /// @brief 위험 판정 로그 rate-limit 용 (차량이 가까이 있는 동안 윈도우마다 반복되므로)
    std::uint64_t riskLogCount_ = 0;
};