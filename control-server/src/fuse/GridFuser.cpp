#include "fuse/GridFuser.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Logger.h"

namespace {

constexpr const char* kIface = "GridFuser";

/// @brief 좌표를 셀 격자 인덱스로 (floor). cellSize > 0 보장됨
std::int32_t cellCoord(double value, double cellSize) {
    return static_cast<std::int32_t>(std::floor(value / cellSize));
}

/// @brief (cx, cy) 셀을 버킷 인덱스로 해시 (Teschner et al. spatial hashing). mask = kBucketCount-1
std::uint32_t cellHash(std::int32_t cx, std::int32_t cy, std::uint32_t mask) {
    const std::uint32_t h =
        (static_cast<std::uint32_t>(cx) * 73856093u) ^ (static_cast<std::uint32_t>(cy) * 19349663u);
    return h & mask;
}

/// @brief 채널 비트. 64채널을 넘으면 마스크로 표현 못하므로 0을 돌려 병합을 막음 (ConcatFuser 와 동일)
std::uint64_t channelBit(veda::ChannelId ch) {
    if (ch < 0 || ch >= 64)
        return 0;
    return std::uint64_t{1} << ch;
}

}  // namespace

GridFuser::GridFuser(std::shared_ptr<IDistanceMetric> metric, double dedupMergeDistance, double trackMaxDistance)
    : metric_(std::move(metric)),
      dedupMergeDistance_(dedupMergeDistance),
      cellSize_(dedupMergeDistance > 0.0 ? dedupMergeDistance : 1.0),
      trackMaxDistance_(trackMaxDistance),
      nextGlobalId_(1) {
    buckets_.resize(kBucketCount);  // 고정 크기 버킷 배열 (이후 재할당 없음)
}

std::size_t GridFuser::ufFind(std::size_t x) {
    while (ufParent_[x] != x) {
        ufParent_[x] = ufParent_[ufParent_[x]];  // path halving
        x = ufParent_[x];
    }
    return x;
}

domain::WorldFrame GridFuser::fuse(const std::vector<domain::ObservationFrame>& frames) {
    domain::WorldFrame worldFrame;

    if (frames.empty()) {
        return worldFrame;
    }

    auto minTimestampIt = std::min_element(
        frames.begin(), frames.end(),
        [](const domain::ObservationFrame& a, const domain::ObservationFrame& b) { return a.ts < b.ts; });
    worldFrame.timestamp = minTimestampIt->ts;

    // --- 후보 수집 (재사용 버퍼) ---
    candidates_.clear();
    std::size_t totalObjects = 0;
    for (const auto& frame : frames) {
        totalObjects += frame.objects.size();
    }
    candidates_.reserve(totalObjects);
    for (const auto& frame : frames) {
        for (const auto& obj : frame.objects) {
            Candidate c;
            c.ch = frame.ch;
            c.id = obj.id;
            c.cls = obj.cls;
            c.pos = obj.pos;  // 이미 월드 좌표 (타입도 domain::WorldPoint)
            candidates_.push_back(c);
        }
    }
    const std::uint32_t n = static_cast<std::uint32_t>(candidates_.size());

    // --- BROAD-PHASE: 공간 해시 그리드로 '근접 쌍'만 수집 -> O(N*k) ---
    // 1) 지난 프레임에 쓴 버킷만 비운다 (capacity 유지 -> 재할당 없음)
    for (std::uint32_t b : touchedBuckets_)
        buckets_[b].clear();
    touchedBuckets_.clear();
    pairs_.clear();

    // 2) 후보를 셀 버킷에 삽입
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::int32_t cx = cellCoord(candidates_[i].pos.x, cellSize_);
        const std::int32_t cy = cellCoord(candidates_[i].pos.y, cellSize_);
        const std::uint32_t b = cellHash(cx, cy, kBucketCount - 1);
        if (buckets_[b].empty())
            touchedBuckets_.push_back(b);
        buckets_[b].push_back(i);
    }

    // 3) 각 후보의 3x3 이웃 셀만 훑어 '근접 + 다른채널 + 같은클래스' 쌍(i<j)을 수집
    //    셀이 dedupMergeDistance 변이므로, 병합 가능한 쌍은 반드시 이 9개 셀 안에 있다.
    for (std::uint32_t i = 0; i < n; ++i) {
        const Candidate& ci = candidates_[i];
        const std::int32_t cx = cellCoord(ci.pos.x, cellSize_);
        const std::int32_t cy = cellCoord(ci.pos.y, cellSize_);

        std::uint32_t seenBuckets[9];  // 이웃 셀들이 같은 버킷으로 해시-충돌하면 중복 스캔 방지
        int seenCount = 0;
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const std::uint32_t b = cellHash(cx + dx, cy + dy, kBucketCount - 1);
                bool dup = false;
                for (int s = 0; s < seenCount; ++s) {
                    if (seenBuckets[s] == b) {
                        dup = true;
                        break;
                    }
                }
                if (dup)
                    continue;
                seenBuckets[seenCount++] = b;

                for (std::uint32_t j : buckets_[b]) {
                    if (j <= i)
                        continue;  // 각 무순서 쌍을 i<j 로 정확히 한 번만
                    const Candidate& cj = candidates_[j];
                    if (ci.ch == cj.ch)
                        continue;
                    if (ci.cls != cj.cls)
                        continue;
                    if (metric_->calculate(ci.pos, cj.pos) > dedupMergeDistance_)
                        continue;  // 해시 충돌로 들어온 먼 후보는 여기서 걸러짐
                    pairs_.emplace_back(i, j);
                }
            }
        }
    }

    // 4) (i,j) 사전순 정렬 -> ConcatFuser 의 전체 O(N^2) 루프와 '동일한 순서'로 병합
    //    (union-find 결과가 순서에 의존할 수 있으므로, 동일 결과를 보장하려면 순서까지 맞춰야 함)
    std::sort(pairs_.begin(), pairs_.end());

    // 5) 채널중복 제약 union-find (재사용 버퍼)
    ufParent_.resize(n);
    ufMask_.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        ufParent_[i] = i;
        ufMask_[i] = channelBit(candidates_[i].ch);
    }
    for (const auto& [i, j] : pairs_) {
        const std::size_t rootI = ufFind(i);
        const std::size_t rootJ = ufFind(j);
        if (rootI == rootJ)
            continue;
        // 두 클러스터가 이미 같은 채널을 품고 있으면 합치지 않음 (한 카메라의 서로 다른 실체 보호)
        if ((ufMask_[rootI] & ufMask_[rootJ]) != 0)
            continue;
        ufParent_[rootI] = rootJ;
        ufMask_[rootJ] |= ufMask_[rootI];
    }

    // --- 이하 cluster 조립 + gid 추적 + coasting: ConcatFuser 와 문자 그대로 동일 ---
    std::vector<std::vector<std::size_t>> clusters(candidates_.size());
    for (std::uint32_t i = 0; i < n; ++i) {
        clusters[ufFind(i)].push_back(i);
    }

    std::vector<domain::WorldObject> fusedObjects;
    std::vector<std::vector<SourceId>> fusedSourceIds;
    fusedObjects.reserve(clusters.size());
    fusedSourceIds.reserve(clusters.size());
    for (const auto& members : clusters) {
        if (members.empty())
            continue;

        domain::WorldObject wObj;
        wObj.cls = candidates_[members.front()].cls;

        std::vector<SourceId> sourceIds;
        sourceIds.reserve(members.size());

        double sumX = 0.0, sumY = 0.0;
        for (std::size_t idx : members) {
            sumX += candidates_[idx].pos.x;
            sumY += candidates_[idx].pos.y;
            wObj.sourceChannels.add(candidates_[idx].ch);
            sourceIds.emplace_back(candidates_[idx].ch, candidates_[idx].id);
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

    if (trackMaxDistance_ > 0.0) {
        std::vector<bool> curMatched(fusedObjects.size(), false);
        std::unordered_set<veda::GlobalId> claimedGids;

        // 1순위: (channel, ObjectId) 가 idIndex_ 에 있으면 그 gid 를 그대로 물려받음
        for (std::size_t c = 0; c < fusedObjects.size(); ++c) {
            for (const auto& sourceId : fusedSourceIds[c]) {
                if (sourceId.second == 0)
                    continue;
                auto idxIt = idIndex_.find(sourceId);
                if (idxIt == idIndex_.end())
                    continue;
                const veda::GlobalId gid = idxIt->second;
                if (claimedGids.count(gid))
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

        // 2순위(fallback): 못 찾은 나머지만, 아직 안 쓰인 gid 들과 거리순 전역 그리디 매칭
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

        for (auto it = idIndex_.begin(); it != idIndex_.end();) {
            if (byGid_.find(it->second) == byGid_.end())
                it = idIndex_.erase(it);
            else
                ++it;
        }

        // 유예 중(이번 윈도우엔 못 봤지만 아직 안 끊긴)인 실체는 마지막 좌표 그대로 채워 넣음(coast)
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

    const std::size_t mergedCount = candidates_.size() - detectedCount;
    if (mergedCount > 0 && isLogEnabled(LogLevel::Debug)) {
        logDebug(kIface, "채널 간 중복 " + std::to_string(mergedCount) + "개 병합 (" +
                             std::to_string(candidates_.size()) + "개 후보 → " + std::to_string(detectedCount) +
                             "개 객체, near-pairs=" + std::to_string(pairs_.size()) + ")");
    }

    return worldFrame;
}
