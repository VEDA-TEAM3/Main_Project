#include "aggregate/TimeWindowAggregator.h"

TimeWindowAggregator::TimeWindowAggregator(std::shared_ptr<IClock> clock, uint64_t windowSizeMs)
    : clock_(std::move(clock)), windowSizeMs_(windowSizeMs), windowStartTime_(0) {}

void TimeWindowAggregator::setCallback(AggregationCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void TimeWindowAggregator::push(const veda::TopViewFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = clock_->now();

    if (buffer_.empty()) {
        windowStartTime_ = now;
    }

    if (now - windowStartTime_ >= windowSizeMs_) {
        if (callback_ && !buffer_.empty()) {
            callback_(buffer_);
        }
        buffer_.clear();
        windowStartTime_ = now;
    }

    buffer_.push_back(frame);
}