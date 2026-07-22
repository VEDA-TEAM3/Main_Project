#pragma once

/**
 * @file    TimeWindowAggregatorV2.h
 * @brief   시간 윈도우 기반 프레임 집계기 (저지연/저할당 최적화 버전)
 *
 * @details
 * 원본 TimeWindowAggregator와 동일한 정책(채널당 이번 윈도우의 최신 프레임 하나만 유지)을
 * 구현하지만, 다음 3가지를 최적화함:
 *  1) 콜백(=다운스트림 파이프라인 전체)을 mutex_ 밖에서 호출
 *     -> 원본은 콜백 실행 중에도 다른 채널의 push()가 전부 블로킹됨
 *  2) 윈도우 마감 시 맵(now 벡터)에서 값을 꺼낼 때 복사 대신 move
 *     -> 어차피 직후 비울 데이터라 복사할 필요가 없음
 *  3) std::unordered_map<ChannelId, TopViewFrame> 대신
 *     std::vector<std::optional<TopViewFrame>> (인덱스 = channelId) 사용
 *     -> 채널 수가 적고 고정이라 해싱 없이 바로 인덱싱, 캐시 지역성도 좋음
 *
 * 원본 TimeWindowAggregator.h/.cpp는 비교/롤백을 위해 그대로 둠
 */

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "interfaces/IClock.h"
#include "interfaces/IFrameAggregator.h"

class TimeWindowAggregatorV2 : public IFrameAggregator {
public:
    /**
     * @brief 생성자
     * @param clock         시스템 시간을 추상화한 시계 인터페이스
     * @param windowSizeMs  프레임을 모을 시간 윈도우 크기 (ms)
     * @param channelCount  채널 개수 (channelId는 [0, channelCount) 범위로 가정)
     */
    TimeWindowAggregatorV2(std::shared_ptr<IClock> clock, uint64_t windowSizeMs, int channelCount);
    ~TimeWindowAggregatorV2() override = default;

    void setCallback(AggregationCallback callback) override;
    void push(const veda::TopViewFrame& frame) override;

private:
    /**
     * @brief   mtx_ 보호 없이 호출됨. 보고 주기(kMetricsReportInterval)가 되면
     *          누적 지표를 리셋하고 로그 문자열을 반환, 아니면 빈 문자열을 반환
     * @details TimeWindowAggregator(V1)와 동일한 항목(평균 락 보유 시간)을 측정해
     *          두 버전을 직접 비교할 수 있게 함
     */
    std::string buildMetricsReportIfDue();

    static constexpr std::chrono::milliseconds kMetricsReportInterval{5000};

    std::shared_ptr<IClock> clock_;
    uint64_t windowSizeMs_;
    int channelCount_;

    std::mutex mutex_;
    AggregationCallback callback_;
    std::vector<std::optional<veda::TopViewFrame>> latestByChannel_;  ///< 인덱스 = channelId
    std::size_t filledCount_ = 0;  ///< 현재 채워진 채널 수 (unordered_map::empty() 대체)
    veda::TimestampMs windowStartTime_ = 0;

    /// @brief 성능 지표 누적 상태 (metricsMutex_로 보호됨, TimeWindowAggregator(V1)와 동일 항목)
    struct Metrics {
        std::uint64_t pushCount = 0;                    ///< push() 호출 횟수
        std::uint64_t windowCount = 0;                  ///< 윈도우 마감(콜백 호출) 횟수
        std::chrono::nanoseconds totalLockHoldTime{0};  ///< push() 1회당 mutex_ 보유 시간 합 (콜백 제외)
        std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    } metrics_;
    std::mutex metricsMutex_;  ///< mutex_와 별개 -> 지표 갱신이 본 로직의 락 경합에 영향 안 줌
};
