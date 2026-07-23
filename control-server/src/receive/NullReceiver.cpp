#include "receive/NullReceiver.h"

#include <chrono>
#include <string>

#include "Logger.h"

namespace {
constexpr const char* kIface = "Receiver";
}  // namespace

NullReceiver::NullReceiver(int channelCount, uint64_t baseIntervalMs, uint64_t jitterStepMs)
    : channelCount_(channelCount), baseIntervalMs_(baseIntervalMs), jitterStepMs_(jitterStepMs) {}

NullReceiver::~NullReceiver() { stop(); }

void NullReceiver::setCallback(FrameCallback callback) { callback_ = std::move(callback); }

void NullReceiver::setAliveCallback(AliveCallback callback) { aliveCallback_ = std::move(callback); }

void NullReceiver::start() {
    if (isRunning_)
        return;
    isRunning_ = true;

    workers_.reserve(static_cast<size_t>(channelCount_));
    for (veda::ChannelId ch = 0; ch < channelCount_; ++ch) {
        workers_.emplace_back(&NullReceiver::channelLoop, this, ch);
    }

    if (aliveCallback_) {
        for (veda::ChannelId ch = 0; ch < channelCount_; ++ch) {
            aliveCallback_(ch, true);
        }
    }

    logSuccess(kIface, "시뮬레이션 채널 " + std::to_string(channelCount_) + "개 시작 (테스트용 NullReceiver)");
}

void NullReceiver::stop() {
    if (!isRunning_)
        return;
    isRunning_ = false;
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    workers_.clear();

    logSuccess(kIface, "시뮬레이션 채널 정지");
}

void NullReceiver::channelLoop(veda::ChannelId ch) {
    const uint64_t intervalMs = baseIntervalMs_ + static_cast<uint64_t>(ch) * jitterStepMs_;

    while (isRunning_) {
        if (callback_) {
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

            veda::TopViewFrame frame;
            frame.ch = ch;
            frame.ts = timestamp;

            veda::TopViewObject obj;
            obj.id = static_cast<int>(ch) * 100 + 1;
            obj.cls = veda::ObjectClass::Vehicle;
            obj.pos.x = static_cast<double>(ch) * 20.0;
            obj.pos.y = static_cast<double>(ch) * 20.0;
            frame.objects.push_back(obj);

            callback_(frame);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }
}