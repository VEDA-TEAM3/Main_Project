#pragma once

/**
 * @file    RtspOnvifSourceV2.h
 * @brief   INetwork → IMetadataSource로 변환하는 어댑터 (저지연/저할당 최적화 버전)
 *
 * @details
 * 원본 RtspOnvifSource와 동일한 재연결/백오프 정책을 쓰지만 다음을 최적화함:
 *  1) std::queue<RawPacket>(deque) -> 고정 크기 링버퍼(kRingCapacity)
 *     : 청크 단위 할당/해제가 없고, 큐가 무제한으로 자라지 않음
 *  2) drop-oldest 정책: 컨슈머(Pipeline)가 못 따라가 링이 가득 차면
 *     가장 오래된 프레임을 버리고 새 프레임을 씀 -> 지연이 무한정 누적되지 않음
 *  3) 콜백마다 새 RawPacket을 만들어 assign()하던 것을 링 슬롯에 직접 assign() ->
 *     슬롯의 vector capacity를 재사용해 콜백당 힙 할당을 없앰
 *  4) next()는 move가 아니라 copy(assign)로 꺼냄
 *     : move를 쓰면 꺼내진 슬롯의 capacity가 함께 빠져나가 다음 write에서
 *       다시 할당해야 하므로(3번의 이득이 사라짐), 대신 memcpy 1회(수 KB, 수십 ns)를
 *       감수하고 슬롯/out 양쪽 다 capacity를 보존함
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/AppConfig.h"
#include "domain/RawPacket.h"
#include "interfaces/IMetadataSource.h"

/**
 * @brief   RtspClientV2의 push 콜백을 pull 인터페이스로 변환하는 어댑터 (최적화 버전)
 *
 * @details
 * 재연결/백오프 정책(1s -> 2s -> ... 최대 30s)은 원본과 동일
 * next()는 재연결 시도 중에도 블로킹 대기할 뿐 false를 반환하지 않음
 */
class RtspOnvifSourceV2 : public IMetadataSource {
public:
    /**
     * @brief   즉시 워커 스레드를 시작
     * @param   config  CCTV 접속 정보
     */
    explicit RtspOnvifSourceV2(const AppConfig& config);

    /**
     * @brief   워커 스레드를 정지시키고 join
     */
    ~RtspOnvifSourceV2() override;

    bool next(domain::RawPacket& out) override;

private:
    /**
     * @brief   [connect->setup->play->run]을 재연결 백오프와 함께 반복하는 워커 루프
     */
    void workerLoop();

    /**
     * @brief   mtx_를 이미 잡은 상태에서 호출. 보고 주기(kMetricsReportInterval)가 되면
     *          누적 지표를 리셋하고 로그 문자열을 반환, 아니면 빈 문자열을 반환
     * @details 실제 로그 출력(logSuccess)은 락을 푼 뒤 호출자가 수행
     */
    std::string buildMetricsReportIfDue();

    /// @brief 링버퍼 용량 (동시에 대기 가능한 최대 프레임 수)
    static constexpr std::size_t kRingCapacity = 8;

    static constexpr std::chrono::milliseconds kMetricsReportInterval{5000};

    AppConfig config_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;

    std::mutex mtx_;
    std::condition_variable cv_;

    /// @brief 고정 크기 링버퍼. 저장소이자 RawPacket 버퍼 풀 역할을 겸함
    std::vector<domain::RawPacket> ring_;
    std::size_t head_ = 0;   ///< 다음에 next()로 꺼낼 위치
    std::size_t count_ = 0;  ///< 현재 채워진 개수 (kRingCapacity 이하)

    /// @brief 성능 지표 누적 상태 (mtx_로 보호됨, RtspOnvifSource(V1)와 동일 항목)
    struct Metrics {
        std::uint64_t producedCount = 0;                ///< 콜백에서 생산된 프레임 수
        std::uint64_t consumedCount = 0;                ///< next()로 소비된 프레임 수
        std::uint64_t droppedCount = 0;                 ///< 링이 가득 차서 버려진 프레임 수 (drop-oldest)
        std::uint64_t totalBytes = 0;                   ///< 소비된 payload 총 바이트 수
        std::chrono::nanoseconds totalQueueLatency{0};  ///< 네트워크 도착(recvTime) ~ next() 인출 시간 합
        std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    } metrics_;
};
