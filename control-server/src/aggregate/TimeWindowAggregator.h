#pragma once

/**
 * @file TimeWindowAggregator.h
 * @brief 시간 윈도우 기반 프레임 집계기 구현체
 *
 * @note
 * - Timestamp를 Server의 ts로 맞춰서 위험
 * -> 패킷에 포함된 Timestamp를 사용하는 것이 더 안전
 */

#include <memory>
#include <mutex>

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
    std::vector<veda::TopViewFrame> buffer_;
    veda::TimestampMs windowStartTime_;
};