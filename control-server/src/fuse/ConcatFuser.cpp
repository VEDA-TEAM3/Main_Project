#include "fuse/ConcatFuser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Logger.h"

namespace {

constexpr const char* kIface = "Fuser";

struct Candidate {
    veda::ChannelId ch = -1;
    veda::ObjectId id = 0;  ///< 채널 내부 ObjectId (ONVIF). 채널 사이에서는 유일하지 않음
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

domain::WorldFrame ConcatFuser::fuse(const std::vector<domain::ObservationFrame>& frames) {
    domain::WorldFrame worldFrame;

    if (frames.empty()) {
        return worldFrame;
    }

    auto minTimestampIt = std::min_element(
        frames.begin(), frames.end(),
        [](const domain::ObservationFrame& a, const domain::ObservationFrame& b) { return a.ts < b.ts; });
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
            c.id = obj.id;
            c.cls = obj.cls;
            c.pos = obj.pos;  // 이미 월드 좌표 (타입도 domain::WorldPoint)
            candidates.push_back(c);
        }
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

    std::vector<domain::WorldObject> fusedObjects;
    std::vector<std::vector<SourceId>> fusedSourceIds;
    fusedObjects.reserve(clusters.size());
    fusedSourceIds.reserve(clusters.size());
    for (const auto& members : clusters) {
        if (members.empty())
            continue;

        domain::WorldObject wObj;
        wObj.cls = candidates[members.front()].cls;

        std::vector<SourceId> sourceIds;
        sourceIds.reserve(members.size());

        double sumX = 0.0, sumY = 0.0;
        for (size_t idx : members) {
            sumX += candidates[idx].pos.x;
            sumY += candidates[idx].pos.y;
            wObj.sourceChannels.add(candidates[idx].ch);
            sourceIds.emplace_back(candidates[idx].ch, candidates[idx].id);
        }
        wObj.pos.x = sumX / static_cast<double>(members.size());
        wObj.pos.y = sumY / static_cast<double>(members.size());

        wObj.gid = 0;
        wObj.riskLevel = veda::RiskLevel::None;
        wObj.nearestObj = 0;
        wObj.nearestDist = -1.0;
        wObj.zoneId = -1;

        fusedObjects.push_back(std::move(wObj));
        fusedSourceIds.push_back(std::move(sourceIds));
    }

    // gid 승계는 trackMaxDistance_ > 0(추적 켬)일 때만 수행
    if (trackMaxDistance_ > 0.0) {
        std::vector<bool> curMatched(fusedObjects.size(), false);
        std::unordered_set<veda::GlobalId> claimedGids;

        // 1순위: (channel, ObjectId) 가 idIndex_ 에 이미 있으면 그 gid 를 그대로 물려받고
        // 좌표만 갱신. 같은 카메라가 같은 실체에 매기는 id 는 이미 안정적이므로, 이 조회
        // 하나로 "직전 프레임"이 아니라 "그 실체를 마지막으로 본 상태"에 바로 도달함
        for (std::size_t c = 0; c < fusedObjects.size(); ++c) {
            for (const auto& sourceId : fusedSourceIds[c]) {
                if (sourceId.second == 0)  // ObjectId 미제공
                    continue;
                auto idxIt = idIndex_.find(sourceId);
                if (idxIt == idIndex_.end())
                    continue;
                const veda::GlobalId gid = idxIt->second;
                if (claimedGids.count(gid))  // 이번 윈도우에 다른 클러스터가 이미 씀
                    continue;
                auto trackIt = byGid_.find(gid);
                if (trackIt == byGid_.end() || trackIt->second.cls != fusedObjects[c].cls)
                    continue;
                if (metric_->calculate(fusedObjects[c].pos, trackIt->second.pos) > trackMaxDistance_)
                    continue;
                fusedObjects[c].gid = gid;
                curMatched[c] = true;
                claimedGids.insert(gid);
                break;
            }
        }

        // 2순위(fallback): 1순위로 못 찾은 나머지만, 아직 안 쓰인 gid 들과 거리순 전역 매칭
        // (채널 간 융합 - ObjectId 비교 불가 - 이나 ONVIF 추적이 끊겨 ObjectId 가 바뀐 경우 대응)
        // 각 객체가 자신과 가장 가까운 후보만 보고 즉시 확정하면 순회 순서에 따라 더 가까운
        // 쌍이 있어도 먼저 처리된 객체가 가로채는 문제가 있어, 가능한 모든 (현재, gid) 쌍을
        // 거리순으로 정렬해 가까운 쌍부터 확정한다
        struct MatchCandidate {
            double dist;
            std::size_t curIdx;
            veda::GlobalId gid;
        };
        std::vector<MatchCandidate> matchCandidates;
        for (std::size_t c = 0; c < fusedObjects.size(); ++c) {
            if (curMatched[c])
                continue;
            for (const auto& [gid, tracked] : byGid_) {
                if (claimedGids.count(gid) || tracked.cls != fusedObjects[c].cls)
                    continue;
                const double dist = metric_->calculate(fusedObjects[c].pos, tracked.pos);
                if (dist > trackMaxDistance_)
                    continue;
                matchCandidates.push_back({dist, c, gid});
            }
        }
        std::sort(matchCandidates.begin(), matchCandidates.end(),
                  [](const MatchCandidate& a, const MatchCandidate& b) { return a.dist < b.dist; });

        for (const auto& match : matchCandidates) {
            if (curMatched[match.curIdx] || claimedGids.count(match.gid))
                continue;
            fusedObjects[match.curIdx].gid = match.gid;
            curMatched[match.curIdx] = true;
            claimedGids.insert(match.gid);
        }
    }

    worldFrame.objects.reserve(fusedObjects.size());
    for (auto& wObj : fusedObjects) {
        if (wObj.gid == 0)
            wObj.gid = nextGlobalId_.fetch_add(1, std::memory_order_relaxed);
        worldFrame.objects.push_back(std::move(wObj));
    }
    const std::size_t detectedCount = worldFrame.objects.size();

    if (trackMaxDistance_ > 0.0) {
        // idIndex_ / byGid_ 갱신 -- 이번 윈도우에 본 실체는 좌표만 최신화하고 missedWindows 리셋
        std::unordered_set<veda::GlobalId> touchedGids;
        for (std::size_t i = 0; i < worldFrame.objects.size(); ++i) {
            const auto& object = worldFrame.objects[i];
            TrackedEntity& entity = byGid_[object.gid];
            entity.cls = object.cls;
            entity.pos = object.pos;
            entity.missedWindows = 0;
            touchedGids.insert(object.gid);
            for (const auto& sourceId : fusedSourceIds[i]) {
                if (sourceId.second != 0)
                    idIndex_[sourceId] = object.gid;
            }
        }

        // 이번 윈도우에 못 본 gid 는 유예 카운터 증가, kMaxMissedWindows 를 넘기면 만료
        // (실체별 카운터라 어떤 실체가 잠깐 안 보여도 다른 실체 추적엔 영향이 없음)
        for (auto it = byGid_.begin(); it != byGid_.end();) {
            if (touchedGids.count(it->first)) {
                ++it;
                continue;
            }
            if (++it->second.missedWindows > kMaxMissedWindows) {
                it = byGid_.erase(it);
            } else {
                ++it;
            }
        }

        // idIndex_ 에서 만료된 gid 를 가리키는 항목 정리
        for (auto it = idIndex_.begin(); it != idIndex_.end();) {
            if (byGid_.find(it->second) == byGid_.end())
                it = idIndex_.erase(it);
            else
                ++it;
        }

        // 유예 중(이번 윈도우엔 못 봤지만 아직 안 끊긴)인 실체는 마지막 좌표 그대로 채워 넣음
        // (안 넣으면 클라이언트가 받는 RiskFrame 에서 이 실체가 잠깐씩 사라졌다 나타나
        //  깜빡이는 것처럼 보임 -- gid 는 안 바뀌어도 화면에 나가는 프레임 자체가 감지
        //  여부에 따라 매 윈도우 갱신되기 때문)
        for (const auto& [gid, entity] : byGid_) {
            if (touchedGids.count(gid))
                continue;
            domain::WorldObject coasted;
            coasted.gid = gid;
            coasted.cls = entity.cls;
            coasted.pos = entity.pos;
            coasted.riskLevel = veda::RiskLevel::None;
            coasted.nearestObj = 0;
            coasted.nearestDist = -1.0;
            coasted.zoneId = -1;
            worldFrame.objects.push_back(std::move(coasted));
        }
    }

    // 윈도우마다 도는 정상 경로라 Debug
    const std::size_t mergedCount = candidates.size() - detectedCount;
    if (mergedCount > 0 && isLogEnabled(LogLevel::Debug)) {
        logDebug(kIface, "채널 간 중복 " + std::to_string(mergedCount) + "개 병합 (" +
                             std::to_string(candidates.size()) + "개 후보 → " +
                             std::to_string(detectedCount) + "개 객체)");
    }

    return worldFrame;
}
