#include "fuse/ConcatFuser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "Logger.h"

namespace {

constexpr const char* kIface = "Fuser";

struct Candidate {
    veda::ChannelId ch = -1;
    veda::ObjectClass cls = veda::ObjectClass::Unknown;
    domain::WorldPoint pos;
};

/**
 * @brief   클러스터별 채널 비트마스크를 함께 관리하는 union-find
 * @details "한 클러스터에 같은 채널이 두 번 들어가면 안 된다"를 O(1)로 판정하기 위함
 *          (예전에는 비교마다 전체 후보를 훑어서 전체 O(n^3)이었음)
 */
class DisjointSet {
public:
    explicit DisjointSet(const std::vector<Candidate>& candidates)
        : parent_(candidates.size()), mask_(candidates.size(), 0) {
        std::iota(parent_.begin(), parent_.end(), 0);
        for (size_t i = 0; i < candidates.size(); ++i)
            mask_[i] = channelBit(candidates[i].ch);
    }

    size_t find(size_t x) {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];
            x = parent_[x];
        }
        return x;
    }

    /// @brief 두 클러스터가 채널을 공유하는가 (합치면 같은 카메라가 두 번 들어가는가)
    bool sharesChannel(size_t rootA, size_t rootB) const { return (mask_[rootA] & mask_[rootB]) != 0; }

    void unite(size_t rootA, size_t rootB) {
        if (rootA == rootB)
            return;
        parent_[rootA] = rootB;
        mask_[rootB] |= mask_[rootA];
    }

private:
    /// @brief 채널 비트. 64채널을 넘으면 마스크로 표현할 수 없으므로 0을 돌려 병합을 막음
    static std::uint64_t channelBit(veda::ChannelId ch) {
        if (ch < 0 || ch >= 64)
            return 0;
        return std::uint64_t{1} << ch;
    }

    std::vector<size_t> parent_;
    std::vector<std::uint64_t> mask_;
};

}  // namespace

ConcatFuser::ConcatFuser(std::shared_ptr<IDistanceMetric> metric, double dedupMergeDistance, double trackMaxDistance)
    : metric_(std::move(metric)),
      dedupMergeDistance_(dedupMergeDistance),
      trackMaxDistance_(trackMaxDistance),
      nextGlobalId_(1) {}

domain::WorldFrame ConcatFuser::fuse(const std::vector<veda::TopViewFrame>& frames) {
    domain::WorldFrame worldFrame;

    if (frames.empty()) {
        return worldFrame;
    }

    auto minTimestampIt =
        std::min_element(frames.begin(), frames.end(),
                         [](const veda::TopViewFrame& a, const veda::TopViewFrame& b) { return a.ts < b.ts; });
    worldFrame.timestamp = minTimestampIt->ts;

    std::size_t totalObjects = 0;
    for (const auto& frame : frames) {
        totalObjects += frame.objects.size();
    }

    std::vector<Candidate> candidates;
    candidates.reserve(totalObjects);
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
        // 관측이 하나도 없는 윈도우 -> 추적을 끊음
        // (오래 비었다가 다시 나타난 객체에 예전 gid 를 물려주면 '같은 실체'라는 보장이 없음)
        previous_.clear();
        return worldFrame;
    }

    DisjointSet ds(candidates);
    for (size_t i = 0; i < candidates.size(); ++i) {
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (candidates[i].ch == candidates[j].ch)
                continue;
            if (candidates[i].cls != candidates[j].cls)
                continue;

            double dist = metric_->calculate(candidates[i].pos, candidates[j].pos);
            if (dist > dedupMergeDistance_)
                continue;

            const size_t rootI = ds.find(i);
            const size_t rootJ = ds.find(j);
            if (rootI == rootJ)
                continue;

            // 두 클러스터가 이미 같은 채널을 품고 있으면 합치지 않음
            // (합치면 한 카메라가 본 서로 다른 실체가 하나로 뭉개짐)
            if (ds.sharesChannel(rootI, rootJ))
                continue;

            ds.unite(rootI, rootJ);
        }
    }

    std::vector<std::vector<size_t>> clusters(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        clusters[ds.find(i)].push_back(i);
    }

    worldFrame.objects.reserve(clusters.size());
    for (const auto& members : clusters) {
        if (members.empty())
            continue;

        domain::WorldObject wObj;
        wObj.cls = candidates[members.front()].cls;
        wObj.sourceChannels.reserve(members.size());

        double sumX = 0.0, sumY = 0.0;
        for (size_t idx : members) {
            sumX += candidates[idx].pos.x;
            sumY += candidates[idx].pos.y;
            wObj.sourceChannels.push_back(candidates[idx].ch);
        }
        wObj.pos.x = sumX / static_cast<double>(members.size());
        wObj.pos.y = sumY / static_cast<double>(members.size());

        // 직전 프레임에서 같은 클래스의 가장 가까운 객체를 찾아 gid 를 물려받음
        // (없으면 새로 발급). 한 번 물려준 트랙은 소비해서 두 객체가 같은 gid 를 갖지 않게 함
        wObj.gid = 0;
        if (trackMaxDistance_ > 0.0) {
            double bestDist = trackMaxDistance_;
            std::size_t bestIdx = previous_.size();
            for (std::size_t p = 0; p < previous_.size(); ++p) {
                if (previous_[p].gid == 0 || previous_[p].cls != wObj.cls)
                    continue;
                const double dist = metric_->calculate(wObj.pos, previous_[p].pos);
                if (dist <= bestDist) {
                    bestDist = dist;
                    bestIdx = p;
                }
            }
            if (bestIdx < previous_.size()) {
                wObj.gid = previous_[bestIdx].gid;
                previous_[bestIdx].gid = 0;  // 소비 처리
            }
        }
        if (wObj.gid == 0)
            wObj.gid = nextGlobalId_.fetch_add(1, std::memory_order_relaxed);

        wObj.riskLevel = veda::RiskLevel::None;
        wObj.nearestObj = 0;
        wObj.nearestDist = -1.0;
        wObj.zoneId = -1;

        worldFrame.objects.push_back(std::move(wObj));
    }

    // 다음 프레임의 gid 승계를 위해 이번 결과를 스냅샷으로 남김
    previous_.clear();
    previous_.reserve(worldFrame.objects.size());
    for (const auto& object : worldFrame.objects)
        previous_.push_back(TrackedObject{object.gid, object.cls, object.pos});

    // 윈도우마다 도는 정상 경로라 Debug
    const std::size_t mergedCount = candidates.size() - worldFrame.objects.size();
    if (mergedCount > 0 && isLogEnabled(LogLevel::Debug)) {
        logDebug(kIface, "채널 간 중복 " + std::to_string(mergedCount) + "개 병합 (" +
                             std::to_string(candidates.size()) + "개 후보 → " +
                             std::to_string(worldFrame.objects.size()) + "개 객체)");
    }

    return worldFrame;
}