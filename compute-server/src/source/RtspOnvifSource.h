#pragma once

/**
 * @file    RtspOnvifSource.h
 * @brief   INetwork → IMetadataSource로 변환하는 어댑터
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "core/AppConfig.h"
#include "domain/RawPacket.h"
#include "interfaces/IMetadataSource.h"

/**
 * @brief   RtspClient의 push 콜백을 pull 인터페이스로 변환하는 어댑터
 *
 * @details
 * RtspClient의 connect -> setup -> play -> run 생명주기를 별도 스레드에서 돌림
 * onPayloadReceived 콜백으로 들어오는 payload 를 스레드 세이프 큐에 쌓고,
 * next()는 그 큐에서 꺼냄
 *
 * 연결이 끊기면(run() 이 리턴하면) 지수 백오프로 재연결
 * (1s -> 2s -> 4s -> ... 최대 30s, 성공 시 리셋)
 * next()는 재연결 시도 중에도 블로킹 대기할 뿐 false를 반환하지 않음
 *
 * @todo
 * - 무제한 큐 방지: OOM 발생할 수 있으므로 old data를 pop()하는 정책 도입 고려
 */
class RtspOnvifSource : public IMetadataSource {
public:
    /**
     * @brief   즉시 워커 스레드를 시작
     * @param   config  CCTV 접속 정보
     */
    explicit RtspOnvifSource(const AppConfig& config);

    /**
     * @brief   워커 스레드를 정지시키고 join
     */
    ~RtspOnvifSource() override;

    bool next(domain::RawPacket& out) override;

    /// @brief 워커 루프와 대기 중인 next()를 깨움 (멱등)
    void stop() noexcept override;

private:
    /**
     * @brief   [connect->setup->play->run]을 재연결 백오프와 함께 반복하는 워커 루프
     */
    void workerLoop();

    /**
     * @brief   mtx_를 이미 잡은 상태에서 호출. 보고 주기(kMetricsReportInterval)가 되면
     *          누적 지표를 리셋하고 로그 문자열을 반환, 아니면 빈 문자열을 반환
     * @details RtspOnvifSourceV2와 동일한 항목을 측정해 두 버전을 직접 비교할 수 있게 함
     *          실제 로그 출력(logSuccess)은 락을 푼 뒤 호출자가 수행
     */
    std::string buildMetricsReportIfDue();

    static constexpr std::chrono::milliseconds kMetricsReportInterval{5000};

    AppConfig config_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<domain::RawPacket> queue_;

    /// @brief 성능 지표 누적 상태 (mtx_로 보호됨)
    struct Metrics {
        std::uint64_t producedCount = 0;  ///< 콜백에서 생산된 프레임 수
        std::uint64_t consumedCount = 0;  ///< next()로 소비된 프레임 수
        std::uint64_t droppedCount = 0;  ///< 큐가 가득 차서 버려진 프레임 수 (V1은 무제한 큐라 항상 0)
        std::uint64_t totalBytes = 0;                   ///< 소비된 payload 총 바이트 수
        std::chrono::nanoseconds totalQueueLatency{0};  ///< 네트워크 도착(recvTime) ~ next() 인출 시간 합
        std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    } metrics_;
};