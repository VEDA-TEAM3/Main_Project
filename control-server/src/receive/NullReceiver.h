#pragma once

/**
 * @file NullReceiver.h
 * @brief 파이프라인 확인용 Null구현체
 */

#include <atomic>
#include <thread>

#include "interfaces/IChannelReceiver.h"

class NullReceiver : public IChannelReceiver {
public:
    NullReceiver() = default;
    ~NullReceiver() override;

    void setCallback(FrameCallback callback) override;
    void setAliveCallback(AliveCallback callback) override;
    void start() override;
    void stop() override;

private:
    void generateDataLoop();

    FrameCallback callback_;
    AliveCallback aliveCallback_;
    std::atomic<bool> isRunning_{false};
    std::thread worker_;
};