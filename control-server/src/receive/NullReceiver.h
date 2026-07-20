#pragma once

/**
 * @file    NullReceiver.h
 * @brief   더미 프레임 데이터를 채널마다 독립된 스레드로 비동기 생성하는 테스트용 구현체
 *
 * @details
 * 채널마다 별도 스레드로 독립적인 타이밍에 push() 를 호출
 * 순차 시뮬레이션으로는 TimeWindowAggregator::push()의 락 경합이
 * 절대 재현되지 않으므로, 이 멀티스레드 구조가 검증의 핵심
 */

#include <atomic>
#include <thread>
#include <vector>

#include "interfaces/IChannelReceiver.h"

class NullReceiver : public IChannelReceiver {
public:
    /**
     * @param channelCount 시뮬레이션할 채널 개수
     * @param baseIntervalMs 채널당 기본 발행 주기
     * @param jitterStepMs 채널 인덱스에 곱해지는 지터
     */
    NullReceiver(int channelCount = 4, uint64_t baseIntervalMs = 2, uint64_t jitterStepMs = 1);
    ~NullReceiver() override;

    void setCallback(FrameCallback callback) override;
    void setAliveCallback(AliveCallback callback) override;
    void start() override;
    void stop() override;

private:
    /**
     * @brief 채널 하나를 담당하는 워커 스레드 루프
     */
    void channelLoop(veda::ChannelId ch);

    int channelCount_;
    uint64_t baseIntervalMs_;
    uint64_t jitterStepMs_;

    FrameCallback callback_;
    AliveCallback aliveCallback_;
    std::atomic<bool> isRunning_{false};
    std::vector<std::thread> workers_;
};