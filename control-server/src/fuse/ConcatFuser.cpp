#include "fuse/ConcatFuser.h"

#include <algorithm>

ConcatFuser::ConcatFuser() : nextGlobalId_(1) {}

domain::WorldFrame ConcatFuser::fuse(const std::vector<veda::TopViewFrame>& frames) {
    domain::WorldFrame worldFrame;

    if (frames.empty()) {
        return worldFrame;
    }

    auto minTimestampIt =
        std::min_element(frames.begin(), frames.end(),
                         [](const veda::TopViewFrame& a, const veda::TopViewFrame& b) { return a.ts < b.ts; });
    worldFrame.timestamp = minTimestampIt->ts;

    for (const auto& frame : frames) {
        for (const auto& obj : frame.objects) {
            domain::WorldObject wObj;

            wObj.gid = nextGlobalId_.fetch_add(1, std::memory_order_relaxed);

            wObj.cls = obj.cls;
            wObj.pos.x = obj.pos.x;
            wObj.pos.y = obj.pos.y;

            wObj.riskLevel = veda::RiskLevel::None;
            wObj.nearestObj = 0;
            wObj.nearestDist = -1.0;

            worldFrame.objects.push_back(wObj);
        }
    }

    return worldFrame;
}