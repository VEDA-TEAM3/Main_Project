#pragma once

/**
 * @file    TimeWindowAggregatorV2.h
 * @brief   시간 윈도우 기반 프레임 집계기 (저지연/무할당)
 *
 * @details
 * 정책: 채널당 이번 윈도우의 '최신 프레임 하나'만 유지하고, 윈도우가 닫히면 묶어서 콜백에 넘긴다.
 *
 * 설계 원칙 3가지 (우선순위 순):
 *  1) 지연  -- 콜백(=다운스트림 파이프라인 전체)을 mutex_ 밖에서 호출한다. 락 안에서 부르면
 *             파이프라인이 도는 내내 다른 채널의 push() 가 전부 블로킹된다.
 *             슬롯은 channelId 로 직접 인덱싱한다(해싱 없음, 캐시 지역성 확보).
 *             윈도우 마감 시에는 activeChannels_ 에 담긴 '채워진 채널만' 순회한다
 *             -- channelCount 가 256 이어도 빈 슬롯을 훑지 않는다.
 *  2) 메모리 -- 핫패스 힙 할당 0. 슬롯의 objects 버퍼는 절대 해제하지 않고 swap 으로
 *             재사용하며, 콜백에 넘길 묶음 버퍼는 FrameBufferPool 에서 빌려 쓴다.
 *  3) 안정성 -- channelId 범위 검사 + 프레임당 객체 수 상한(kMaxObjectsPerFrame)으로
 *             원격 입력이 메모리를 무한히 밀어 넣지 못하게 막는다.
 *
 * @note [ 버퍼 회전 ]
 * 마감 시 slot.objects 와 pool 버퍼의 objects 를 swap 한다. 슬롯은 풀 버퍼가 갖고 있던
 * (비어 있지만 capacity 는 살아 있는) 버퍼를 받고, 풀 버퍼는 데이터를 가져간다.
 * 버퍼가 두 곳을 오갈 뿐 해제되지 않으므로 warmup 이후 할당이 0이 된다.
 */

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "aggregate/FrameBufferPool.h"
#include "interfaces/IClock.h"
#include "interfaces/IFrameAggregator.h"

class TimeWindowAggregatorV2 : public IFrameAggregator {
public:
    /**
     * @brief 프레임 1개가 담을 수 있는 객체 수 상한
     *
     * @details
     * compute-server 의 OnvifParser/ContainmentSanitizer 상한과 같은 값이어야 한다.
     * 그쪽이 이미 256 으로 자르지만, control-server 는 '신뢰할 수 없는 원격 입력'을 받는
     * 쪽이므로 자기 방어를 따로 한다 -- 저쪽 상한을 믿는 것은 경계 검사가 아니다.
     */
    static constexpr std::size_t kMaxObjectsPerFrame = 256;

    /**
     * @param clock         시스템 시간을 추상화한 시계 인터페이스
     * @param windowSizeMs  프레임을 모을 시간 윈도우 크기 (ms)
     * @param channelCount  채널 개수 (channelId 는 [0, channelCount) 로 가정)
     * @throws std::invalid_argument channelCount 가 [1, kMaxChannelCount] 밖이거나 clock 이 null
     */
    TimeWindowAggregatorV2(std::shared_ptr<IClock> clock, uint64_t windowSizeMs, int channelCount);
    ~TimeWindowAggregatorV2() override = default;

    void setCallback(AggregationCallback callback) override;
    void push(const veda::TopViewFrame& frame) override;

private:
    /// @brief 슬롯 -> 풀 버퍼로 swap 해 묶음을 만든다 (mutex_ 를 쥔 채 호출)
    void fillFlushBufferLocked(FrameBufferPool::Buffer& out);

    /// @brief 채워진 슬롯을 비운다. capacity 는 유지 (mutex_ 를 쥔 채 호출)
    void clearSlotsLocked();

    /**
     * @brief   보고 주기가 되면 누적 지표를 리셋하고 로그 문자열을 반환, 아니면 빈 문자열
     * @param   now push() 가 이미 측정해 둔 시각 -- 여기서 다시 clock 을 읽지 않는다
     */
    std::string buildMetricsReportIfDue(std::chrono::steady_clock::time_point now);

    static constexpr std::chrono::milliseconds kMetricsReportInterval{5000};

    /// @brief 동시에 살아 있을 수 있는 마감 버퍼 수. 실제 배포는 push() 가 단일 스레드라 1이면 충분하나,
    ///        멀티스레드 Receiver(테스트/모의) 를 위해 여유를 둔다
    static constexpr std::size_t kFlushBufferPoolSize = 4;

    std::shared_ptr<IClock> clock_;
    uint64_t windowSizeMs_;
    int channelCount_;

    std::mutex mutex_;
    AggregationCallback callback_;

    /// @brief 인덱스 = channelId. optional 대신 평범한 벡터 -- reset() 이 objects 버퍼를
    ///        해제해 버리는 것을 막고, 점유 여부는 occupied_ 로 따로 본다
    std::vector<veda::TopViewFrame> slots_;
    std::vector<std::uint8_t> occupied_;         ///< 인덱스 = channelId (0/1)
    std::vector<veda::ChannelId> activeChannels_;  ///< 이번 윈도우에 채워진 채널만 (마감 시 순회 대상)
    veda::TimestampMs windowStartTime_ = 0;

    FrameBufferPool flushPool_;

    /// @brief 성능 지표 누적 상태 (metricsMutex_ 로 보호)
    struct Metrics {
        std::uint64_t pushCount = 0;                    ///< push() 호출 횟수
        std::uint64_t windowCount = 0;                  ///< 윈도우 마감(콜백 호출) 횟수
        std::uint64_t droppedCount = 0;                 ///< 범위/상한 위반으로 버린 프레임 수
        std::chrono::nanoseconds totalLockHoldTime{0};  ///< push() 1회당 mutex_ 보유 시간 합 (콜백 제외)
        std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    } metrics_;
    std::mutex metricsMutex_;  ///< mutex_ 와 별개 -> 지표 갱신이 본 로직의 락 경합에 영향 안 줌
};
