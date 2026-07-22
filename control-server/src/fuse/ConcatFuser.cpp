#include "fuse/ConcatFuser.h"

#include <algorithm>
#include <numeric>
#include <vector>

namespace {

struct Candidate {
    veda::ChannelId ch = -1;
    veda::ObjectClass cls = veda::ObjectClass::Unknown;
    domain::WorldPoint pos;
};

class DisjointSet {
public:
    explicit DisjointSet(size_t n) : parent_(n) { std::iota(parent_.begin(), parent_.end(), 0); }

    size_t find(size_t x) {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];
            x = parent_[x];
        }
        return x;
    }

    void unite(size_t a, size_t b) {
        a = find(a);
        b = find(b);
        if (a != b)
            parent_[a] = b;
    }

private:
    std::vector<size_t> parent_;
};

bool clusterHasChannel(const std::vector<Candidate>& candidates, DisjointSet& ds, size_t root, veda::ChannelId ch) {
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (ds.find(i) == root && candidates[i].ch == ch) {
            return true;
        }
    }
    return false;
}

}  // namespace

ConcatFuser::ConcatFuser(std::shared_ptr<IDistanceMetric> metric, double dedupMergeDistance)
    : metric_(std::move(metric)), dedupMergeDistance_(dedupMergeDistance), nextGlobalId_(1) {}

domain::WorldFrame ConcatFuser::fuse(const std::vector<veda::TopViewFrame>& frames) {
    domain::WorldFrame worldFrame;

    if (frames.empty()) {
        return worldFrame;
    }

    auto minTimestampIt =
        std::min_element(frames.begin(), frames.end(),
                         [](const veda::TopViewFrame& a, const veda::TopViewFrame& b) { return a.ts < b.ts; });
    worldFrame.timestamp = minTimestampIt->ts;

    std::vector<Candidate> candidates;
    for (const auto& frame : frames) {
        for (const auto& obj : frame.objects) {
            Candidate c;
            c.ch = frame.ch;
            c.cls = obj.cls;
            c.pos.x = obj.pos.x;
            c.pos.y = obj.pos.y;
            candidates.push_back(c);
        }
    }

    if (candidates.empty()) {
        return worldFrame;
    }

    DisjointSet ds(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (candidates[i].ch == candidates[j].ch)
                continue;
            if (candidates[i].cls != candidates[j].cls)
                continue;

            double dist = metric_->calculate(candidates[i].pos, candidates[j].pos);
            if (dist > dedupMergeDistance_)
                continue;

            size_t rootI = ds.find(i);
            size_t rootJ = ds.find(j);
            if (rootI == rootJ)
                continue;

            if (clusterHasChannel(candidates, ds, rootI, candidates[j].ch) ||
                clusterHasChannel(candidates, ds, rootJ, candidates[i].ch)) {
                continue;
            }

            ds.unite(i, j);
        }
    }

    std::vector<std::vector<size_t>> clusters(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        clusters[ds.find(i)].push_back(i);
    }

    for (const auto& members : clusters) {
        if (members.empty())
            continue;

        domain::WorldObject wObj;
        wObj.gid = nextGlobalId_.fetch_add(1, std::memory_order_relaxed);
        wObj.cls = candidates[members.front()].cls;

        double sumX = 0.0, sumY = 0.0;
        for (size_t idx : members) {
            sumX += candidates[idx].pos.x;
            sumY += candidates[idx].pos.y;
            wObj.sourceChannels.push_back(candidates[idx].ch);
        }
        wObj.pos.x = sumX / static_cast<double>(members.size());
        wObj.pos.y = sumY / static_cast<double>(members.size());

        wObj.riskLevel = veda::RiskLevel::None;
        wObj.nearestObj = 0;
        wObj.nearestDist = -1.0;
        wObj.zoneId = -1;

        worldFrame.objects.push_back(wObj);
    }

    return worldFrame;
}