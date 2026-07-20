#pragma once

/**
 * @file    TimeWindowAggregator.h
 * @brief   시간 윈도우 기반 프레임 집계기 구현체
 *
 * @details
 * 채널당 이번 윈도우의 최신 프레임 하나만 유지
 * 단순 누적이 아닌 이유: compute-server의 프레임 발행 주기가 windowSizeMs 보다 짧으면,
 * 같은 채널의 시간차 중복 프레임이 한 윈도우에 여러 개 들어가 ConcatFuser의 dedup 범위 밖에서 오염 발생
 * 같은 채널 내 시간차 중복이 서로 다른 물리적 객체로 오인되어 위험도 판정을 왜곡
 */

#include <memory>
#include <mutex>
#include <unordered_map>

#include "interfaces/IClock.h"
#include "interfaces/IFrameAggregator.h"

class TimeWindowAggregator : public IFrameAggregator {
public:
    /**
     * @brief 생성자
     * @param clock 시스템 시간을 추상화한 시계 인터페이스
     * @param windowSizeMs 프레임을 모을 시간 윈도우 크기 (ms)
     */
    TimeWindowAggregator(std::shared_ptr<IClock> clock, uint64_t windowSizeMs);
    ~TimeWindowAggregator() override = default;

    void setCallback(AggregationCallback callback) override;
    void push(const veda::TopViewFrame& frame) override;

private:
    std::shared_ptr<IClock> clock_;
    uint64_t windowSizeMs_;

    std::mutex mutex_;
    AggregationCallback callback_;
    std::unordered_map<veda::ChannelId, veda::TopViewFrame> latestByChannel_;  ///< 채널당 최신 프레임 1개
    veda::TimestampMs windowStartTime_;
};