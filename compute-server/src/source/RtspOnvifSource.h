#pragma once

/**
 * @file    RtspOnvifSource.h
 * @brief   INetwork → IMetadataSource로 변환하는 어댑터
 */

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
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

private:
    /**
     * @brief   [connect->setup->play->run]을 재연결 백오프와 함께 반복하는 워커 루프
     */
    void workerLoop();

    AppConfig config_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<domain::RawPacket> queue_;
};