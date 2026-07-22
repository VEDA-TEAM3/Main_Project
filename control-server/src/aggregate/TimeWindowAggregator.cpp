#include "aggregate/TimeWindowAggregator.h"

#include <iostream>

TimeWindowAggregator::TimeWindowAggregator(std::shared_ptr<IClock> clock, uint64_t windowSizeMs)
    : clock_(std::move(clock)), windowSizeMs_(windowSizeMs), windowStartTime_(0) {}

void TimeWindowAggregator::setCallback(AggregationCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void TimeWindowAggregator::push(const veda::TopViewFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = clock_->now();

    if (latestByChannel_.empty()) {
        windowStartTime_ = now;
    }

    if (now - windowStartTime_ >= windowSizeMs_) {
        if (callback_ && !latestByChannel_.empty()) {
            std::vector<veda::TopViewFrame> frames;
            frames.reserve(latestByChannel_.size());
            for (const auto& [ch, f] : latestByChannel_) {
                frames.push_back(f);
            }
            callback_(std::move(frames));
        }
        latestByChannel_.clear();
        windowStartTime_ = now;
    }

    latestByChannel_[frame.ch] = frame;
}