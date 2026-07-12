/**
 * @file    FrameAggregator.cpp
 * @brief   FrameAggregator 구현부
 */
#include "core/FrameAggregator.h"

namespace {

/**
 * @brief   channelId와 objectId를 조합하여 버퍼 키를 생성한다.
 */
std::string makeKey(const std::string& channelId, int objectId) { return channelId + "_" + std::to_string(objectId); }

}  // namespace

void FrameAggregator::push(const TrackedPoint& point) {
    const std::string key = makeKey(point.channelId, point.objectId);

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = lastSeenTimestampByKey_.find(key);
    if (it != lastSeenTimestampByKey_.end() && point.timestamp <= it->second) {
        return;
    }

    lastSeenTimestampByKey_[key] = point.timestamp;
    latestByKey_[key] = point;
}

std::vector<TrackedPoint> FrameAggregator::collectFrame() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<TrackedPoint> frame;
    frame.reserve(latestByKey_.size());
    for (const auto& [key, point] : latestByKey_) {
        frame.push_back(point);
    }

    latestByKey_.clear();

    return frame;
}