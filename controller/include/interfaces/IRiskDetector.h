/**
 * @file    IRiskDetector.h
 * @brief   TrackedPoint 목록으로부터 채널별 위험 단계를 판단하는 인터페이스
 * @note    8방향 최단거리 계산 알고리즘이 향후 변경될 경우를 대비해 추상화한다.
 *          구현체가 실제 판단 로직을 담당한다.
 */
#pragma once

#include <vector>

#include "model/RiskResult.h"
#include "model/TrackedPoint.h"

/**
 * @brief   위험 판단 인터페이스
 */
class IRiskDetector {
public:
    virtual ~IRiskDetector() = default;

    /**
     * @brief   한 시점(FrameAggregator가 만든 스냅샷)의 TrackedPoint 목록을 평가하여
     *          채널별 위험 단계를 산출한다.
     * @note    거리 계산은 채널 경계와 무관하게 frame 전체를 대상으로 이루어지며,
     *          그 결과로 얻어진 위험 쌍에 관여된 모든 채널의 level이 함께 승격된다
     *          (RiskResult::ChannelStatus 참고).
     * @param   frame   같은 시점으로 동기화된 전체 채널의 TrackedPoint 목록
     * @return  채널별 위험 판단 결과
     */
    virtual RiskResult evaluate(const std::vector<TrackedPoint>& frame) = 0;
};