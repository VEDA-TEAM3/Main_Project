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
 *
 * [ 핫패스 설계 ]
 * 최근접 탐색은 차량 수 V × 객체 수 N 의 이중 루프다. 세 가지를 지킨다.
 *  1) 수치 안정성 -- 비유한 좌표를 O(N) 사전 검사에서 걸러 이중 루프로 들여보내지 않는다.
 *     NaN 은 모든 비교에서 false 라 '경보가 조용히 안 나가는' 최악 방향으로 실패한다.
 *  2) 캐시 지역성 -- 이중 루프가 훑는 것은 WorldObject(약 80B) 전체가 아니라
 *     좌표만 모은 연속 배열(candPos_, 16B/개)이다.
 *  3) 무할당 -- 모든 스크래치 버퍼가 멤버이며 clear()/resize() 로 capacity 를 재사용한다.
 *     결과(RiskEvaluation)도 호출자 소유 out-parameter 로 받는다.
 */

#include <cstdint>
#include <memory>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/IDistanceMetric.h"
#include "interfaces/IRiskPolicy.h"

class ThresholdRiskPolicy : public IRiskPolicy {
public:
    /**
     * @brief 생성자
     * @param metric       거리 계산기
     * @param risk         거리 임계값 (AppConfig::risk)
     * @param channelCount 채널(zone) 개수
     *
     * @throws std::invalid_argument
     *         metric 이 null, channelCount 가 [1, 256] 밖,
     *         임계값이 비유한/음수, 또는 dangerousDistance > warningDistance 인 경우
     */
    ThresholdRiskPolicy(std::shared_ptr<IDistanceMetric> metric, const RiskConfig& risk, int channelCount);
    ~ThresholdRiskPolicy() override = default;

    void evaluate(domain::WorldFrame& frame, domain::RiskEvaluation& out) override;

private:
    /// @brief 최근접 대상을 찾지 못했음을 나타내는 인덱스 (gid==0 센티널을 대체)
    static constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

    std::shared_ptr<IDistanceMetric> metric_;
    double warningDistance_;
    double dangerousDistance_;
    int channelCount_;

    /**
     * @brief 이중 루프 전용 좌표 배열 (Structure of Arrays)
     *
     * @details
     * WorldObject 는 gid/cls/riskLevel/nearestObj/nearestDist/zoneId/sourceChannels 까지 안고 있어
     * 약 80B 다. 최근접 탐색이 실제로 읽는 것은 pos(16B) 뿐인데 객체 배열을 그대로 훑으면
     * 캐시 라인마다 쓸모없는 64B 를 함께 끌어온다. 좌표만 따로 모아 밀도를 5배로 올린다.
     * candIdx_[k] 는 candPos_[k] 가 frame.objects 의 몇 번째인지를 돌려주는 역인덱스다.
     */
    std::vector<domain::WorldPoint> candPos_;
    std::vector<std::uint32_t> candIdx_;

    /// @brief 위험 판정 로그 rate-limit 용 (차량이 가까이 있는 동안 윈도우마다 반복되므로)
    std::uint64_t riskLogCount_ = 0;

    /// @brief 비유한 좌표로 폐기한 객체 수 (rate-limit 로그용, 누적)
    std::uint64_t nonFiniteCount_ = 0;
};
