#include "fuse/GridFuser.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Logger.h"

namespace {

constexpr const char* kIface = "GridFuser";
// 작은 프레임은 3x3 셀 탐색 준비 비용이 전체 쌍 비교보다 크다.
// 동일한 union-find/추적 상태를 유지한 채 이 지점부터 공간 그리드를 사용한다.
constexpr std::uint32_t kMinGridCandidateCount = 96;

/// @brief 좌표를 셀 격자 인덱스로 (floor). cellSize > 0 보장됨
std::int32_t cellCoord(double value, double cellSize) {
    return static_cast<std::int32_t>(std::floor(value / cellSize));
}

/// @brief (cx, cy) 셀을 버킷 인덱스로 해시 (Teschner et al. spatial hashing). mask = kBucketCount-1
std::uint32_t cellHash(std::int32_t cx, std::int32_t cy, std::uint32_t mask) {
    const std::uint32_t h = (static_cast<std::uint32_t>(cx) * 73856093u) ^ (static_cast<std::uint32_t>(cy) * 19349663u);
    return h & mask;
}

/// @brief floor(value / cellSize)를 int32_t로 안전하게 표현할 수 있는지 확인한다.
bool canRepresentCell(double value, double cellSize) {
    if (!std::isfinite(value)) {
        return false;
    }
    const double coordinate = std::floor(value / cellSize);
    return std::isfinite(coordinate) && coordinate >= static_cast<double>(std::numeric_limits<std::int32_t>::min()) &&
           coordinate <= static_cast<double>(std::numeric_limits<std::int32_t>::max());
}

/// @brief 채널 비트. 64채널을 넘으면 마스크로 표현 못하므로 0을 돌려 병합을 막음 (ConcatFuser 와 동일)
std::uint64_t channelBit(veda::ChannelId ch) {
    if (ch < 0 || ch >= 64) {
        return 0;
    }
    return std::uint64_t{1} << ch;
}

/**
 * @brief 원시 좌표 주위에 반경 radius 의 공간 히스테리시스를 적용한다.
 *
 * 이전 출력에서 radius 안쪽의 변화는 정지 잡음으로 보고 고정한다. 반경 밖으로 이동하면
 * 원시 좌표 방향으로 따라가되 출력과 원시 좌표 사이 거리가 정확히 radius 가 되게 한다.
 * 따라서 EMA처럼 속도에 따라 시간 지연이 계속 누적되지 않고 공간 오차 상한이 radius 로 제한된다.
 */
domain::WorldPoint stabilizePosition(const domain::WorldPoint& previous, const domain::WorldPoint& raw, double radius) {
    if (!(radius > 0.0) || !std::isfinite(previous.x) || !std::isfinite(previous.y) || !std::isfinite(raw.x) ||
        !std::isfinite(raw.y)) {
        return raw;
    }

    const double dx = raw.x - previous.x;
    const double dy = raw.y - previous.y;
    const double distance = std::hypot(dx, dy);
    if (distance <= radius) {
        return previous;
    }
    const double followScale = (distance - radius) / distance;
    return {previous.x + dx * followScale, previous.y + dy * followScale};
}

}  // namespace

GridFuser::GridFuser(std::shared_ptr<IDistanceMetric> metric, double dedupMergeDistance, double trackMaxDistance,
                     double positionJitterRadius)
    : metric_(std::move(metric)),
      dedupMergeDistance_(dedupMergeDistance),
      cellSize_(dedupMergeDistance > 0.0 ? dedupMergeDistance : 1.0),
      trackMaxDistance_(trackMaxDistance),
      positionJitterRadius_(std::isfinite(positionJitterRadius) && positionJitterRadius > 0.0 ? positionJitterRadius
                                                                                              : 0.0),
      nextGlobalId_(1) {}

std::size_t GridFuser::ufFind(std::size_t x) {
    while (ufParent_[x] != x) {
        ufParent_[x] = ufParent_[ufParent_[x]];  // path halving
        x = ufParent_[x];
    }
    return x;
}

domain::WorldFrame GridFuser::fuse(const std::vector<domain::ObservationFrame>& frames) {
    domain::WorldFrame worldFrame;

    if (!frames.empty()) {
        const auto maxTimestampIt = std::max_element(
            frames.begin(), frames.end(),
            [](const domain::ObservationFrame& a, const domain::ObservationFrame& b) { return a.ts < b.ts; });
        worldFrame.timestamp = maxTimestampIt->ts;
    }
    const std::uint64_t motionFrame = ++motionFrame_;

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
    // 2) 채널중복 제약 union-find 초기화. 작은 프레임의 직접 비교 경로와 그리드 경로가 공유한다.
    ufParent_.resize(n);
    ufMask_.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        ufParent_[i] = i;
        ufMask_[i] = channelBit(candidates_[i].ch);
    }

    bool useGrid = n >= kMinGridCandidateCount && !std::isnan(dedupMergeDistance_);
    if (useGrid) {
        for (const auto& candidate : candidates_) {
            if (!canRepresentCell(candidate.pos.x, cellSize_) || !canRepresentCell(candidate.pos.y, cellSize_)) {
                useGrid = false;
                break;
            }
        }
    }

    if (useGrid) {
        // 지난 그리드 프레임에 쓴 버킷만 비운다. 그 사이 작은 프레임이 실행됐어도
        // touchedBuckets_를 보존하므로 다음 그리드 진입 시 정확히 정리된다.
        for (std::uint32_t b : touchedBuckets_) {
            buckets_[b].clear();
        }
        touchedBuckets_.clear();
        pairs_.clear();

        if (buckets_.empty()) {
            buckets_.resize(kBucketCount);  // 큰 프레임이 실제로 들어올 때 한 번만 할당
        }

        // 2) 후보를 셀 버킷에 삽입
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::int32_t cx = cellCoord(candidates_[i].pos.x, cellSize_);
            const std::int32_t cy = cellCoord(candidates_[i].pos.y, cellSize_);
            const std::uint32_t b = cellHash(cx, cy, kBucketCount - 1);
            if (buckets_[b].empty()) {
                touchedBuckets_.push_back(b);
            }
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
                    if (dup) {
                        continue;
                    }
                    seenBuckets[seenCount++] = b;

                    for (std::uint32_t j : buckets_[b]) {
                        if (j <= i) {
                            continue;  // 각 무순서 쌍을 i<j 로 정확히 한 번만
                        }
                        const Candidate& cj = candidates_[j];
                        if (ci.ch == cj.ch) {
                            continue;
                        }
                        if (ci.cls != cj.cls) {
                            continue;
                        }
                        if (!std::isfinite(ci.pos.x) || !std::isfinite(ci.pos.y) || !std::isfinite(cj.pos.x) ||
                            !std::isfinite(cj.pos.y)) {
                            continue;
                        }
                        if (metric_->calculate(ci.pos, cj.pos) > dedupMergeDistance_) {
                            continue;  // 해시 충돌로 들어온 먼 후보는 여기서 걸러짐
                        }
                        pairs_.emplace_back(i, j);
                    }
                }
            }
        }

        // (i,j) 사전순 정렬 -> ConcatFuser 의 전체 O(N^2) 루프와 '동일한 순서'로 병합한다.
        // union-find 결과가 순서에 의존할 수 있으므로 결과 보존을 위해 순서까지 맞춘다.
        std::sort(pairs_.begin(), pairs_.end());
        for (const auto& [i, j] : pairs_) {
            const std::size_t rootI = ufFind(i);
            const std::size_t rootJ = ufFind(j);
            if (rootI == rootJ) {
                continue;
            }
            if ((ufMask_[rootI] & ufMask_[rootJ]) != 0) {
                continue;
            }
            ufParent_[rootI] = rootJ;
            ufMask_[rootJ] |= ufMask_[rootI];
        }
    } else {
        // 작은 프레임 또는 셀로 안전하게 표현할 수 없는 입력은 전체 쌍 비교를 사용한다.
        // ConcatFuser와 같은 순서로 즉시 union하여 정렬/후보 저장 비용도 만들지 않는다.
        for (std::uint32_t i = 0; i < n; ++i) {
            for (std::uint32_t j = i + 1; j < n; ++j) {
                if (candidates_[i].ch == candidates_[j].ch) {
                    continue;
                }
                if (candidates_[i].cls != candidates_[j].cls) {
                    continue;
                }
                if (!std::isfinite(candidates_[i].pos.x) || !std::isfinite(candidates_[i].pos.y) ||
                    !std::isfinite(candidates_[j].pos.x) || !std::isfinite(candidates_[j].pos.y)) {
                    continue;
                }
                if (metric_->calculate(candidates_[i].pos, candidates_[j].pos) > dedupMergeDistance_) {
                    continue;
                }
                const std::size_t rootI = ufFind(i);
                const std::size_t rootJ = ufFind(j);
                if (rootI == rootJ) {
                    continue;
                }
                if ((ufMask_[rootI] & ufMask_[rootJ]) != 0) {
                    continue;
                }
                ufParent_[rootI] = rootJ;
                ufMask_[rootJ] |= ufMask_[rootI];
            }
        }
    }

    // --- cluster 조립 + gid 추적은 ConcatFuser 규칙을 보존하고, 출력 직전에만 좌표를 안정화 ---
    std::vector<std::vector<std::size_t>> clusters(candidates_.size());
    for (std::uint32_t i = 0; i < n; ++i) {
        clusters[ufFind(i)].push_back(i);
    }

    std::vector<domain::WorldObject> fusedObjects;
    std::vector<std::vector<SourceId>> fusedSourceIds;
    fusedObjects.reserve(clusters.size());
    fusedSourceIds.reserve(clusters.size());
    for (const auto& members : clusters) {
        if (members.empty()) {
            continue;
        }

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

    std::vector<bool> ambiguousMatches;
    if (trackMaxDistance_ > 0.0) {
        std::vector<bool> curMatched(fusedObjects.size(), false);
        ambiguousMatches.resize(fusedObjects.size(), false);
        std::unordered_set<veda::GlobalId> claimedGids;

        const auto predictPosition = [this, motionFrame](const TrackedEntity& tracked) {
            domain::WorldPoint predicted = tracked.rawPos;
            if (!tracked.hasMotion || motionFrame <= tracked.lastMotionFrame || !std::isfinite(tracked.motionDelta.x) ||
                !std::isfinite(tracked.motionDelta.y)) {
                return predicted;
            }

            const double elapsedFrames = static_cast<double>(motionFrame - tracked.lastMotionFrame);
            double dx = tracked.motionDelta.x * elapsedFrames;
            double dy = tracked.motionDelta.y * elapsedFrames;
            predicted.x += dx;
            predicted.y += dy;
            return predicted;
        };

        const double ambiguityMargin = std::max(positionJitterRadius_, 1.0e-6);
        for (std::size_t lhs = 0; lhs < fusedObjects.size(); ++lhs) {
            for (std::size_t rhs = lhs + 1; rhs < fusedObjects.size(); ++rhs) {
                if (fusedObjects[lhs].cls != fusedObjects[rhs].cls) {
                    continue;
                }
                const double distance = metric_->calculate(fusedObjects[lhs].pos, fusedObjects[rhs].pos);
                if (std::isfinite(distance) && distance <= ambiguityMargin) {
                    ambiguousMatches[lhs] = true;
                    ambiguousMatches[rhs] = true;
                }
            }
        }

        // 1순위: 명확한 구간은 source id를 쓰되 이동 방향과 모순되면 fallback에 맡긴다.
        // 모호 구간도 이동 이력이 없으면 source id를 우선한다.
        for (std::size_t c = 0; c < fusedObjects.size(); ++c) {
            for (const auto& sourceId : fusedSourceIds[c]) {
                if (sourceId.second == 0) {
                    continue;
                }
                auto idxIt = idIndex_.find(sourceId);
                if (idxIt == idIndex_.end()) {
                    continue;
                }
                const veda::GlobalId gid = idxIt->second;
                if (claimedGids.count(gid)) {
                    continue;
                }
                auto trackIt = byGid_.find(gid);
                if (trackIt == byGid_.end() || trackIt->second.cls != fusedObjects[c].cls) {
                    continue;
                }
                if (ambiguousMatches[c] && trackIt->second.hasMotion) {
                    continue;
                }
                const double rawDistance = metric_->calculate(fusedObjects[c].pos, trackIt->second.rawPos);
                const double predictedDistance =
                    metric_->calculate(fusedObjects[c].pos, predictPosition(trackIt->second));
                if (!std::isfinite(rawDistance) || !std::isfinite(predictedDistance) ||
                    predictedDistance > trackMaxDistance_) {
                    continue;
                }

                bool contradictedByMotion = false;
                for (const auto& [otherGid, otherTrack] : byGid_) {
                    if (otherGid == gid || claimedGids.count(otherGid) || otherTrack.cls != fusedObjects[c].cls) {
                        continue;
                    }
                    if (!trackIt->second.hasMotion || !otherTrack.hasMotion) {
                        continue;
                    }
                    const double otherRawDistance = metric_->calculate(fusedObjects[c].pos, otherTrack.rawPos);
                    const double otherPredictedDistance =
                        metric_->calculate(fusedObjects[c].pos, predictPosition(otherTrack));
                    if (std::isfinite(otherRawDistance) && std::isfinite(otherPredictedDistance) &&
                        otherRawDistance <= trackMaxDistance_ && otherPredictedDistance < predictedDistance) {
                        contradictedByMotion = true;
                        break;
                    }
                }
                if (contradictedByMotion) {
                    continue;
                }
                fusedObjects[c].gid = gid;
                curMatched[c] = true;
                claimedGids.insert(gid);
                break;
            }
        }

        // 2순위(fallback): 최근 이동 방향으로 예측한 좌표를 기준으로 전역 그리디 매칭
        struct MatchCandidate {
            double dist;
            std::size_t curIdx;
            veda::GlobalId gid;
        };
        std::vector<MatchCandidate> matchCandidates;
        for (std::size_t c = 0; c < fusedObjects.size(); ++c) {
            if (curMatched[c]) {
                continue;
            }
            for (const auto& [gid, tracked] : byGid_) {
                if (claimedGids.count(gid) || tracked.cls != fusedObjects[c].cls) {
                    continue;
                }
                const double rawDistance = metric_->calculate(fusedObjects[c].pos, tracked.rawPos);
                const double predictedDistance = metric_->calculate(fusedObjects[c].pos, predictPosition(tracked));
                if (!std::isfinite(rawDistance) || !std::isfinite(predictedDistance) ||
                    predictedDistance > trackMaxDistance_) {
                    continue;
                }
                matchCandidates.push_back({predictedDistance, c, gid});
            }
        }
        std::sort(matchCandidates.begin(), matchCandidates.end(), [](const MatchCandidate& a, const MatchCandidate& b) {
            if (a.dist != b.dist) {
                return a.dist < b.dist;
            }
            if (a.curIdx != b.curIdx) {
                return a.curIdx < b.curIdx;
            }
            return a.gid < b.gid;
        });

        for (const auto& match : matchCandidates) {
            if (curMatched[match.curIdx] || claimedGids.count(match.gid)) {
                continue;
            }
            fusedObjects[match.curIdx].gid = match.gid;
            curMatched[match.curIdx] = true;
            claimedGids.insert(match.gid);
        }
    }

    worldFrame.objects.reserve(fusedObjects.size());
    for (auto& wObj : fusedObjects) {
        if (wObj.gid == 0) {
            wObj.gid = nextGlobalId_.fetch_add(1, std::memory_order_relaxed);
        }
        worldFrame.objects.push_back(std::move(wObj));
    }
    const std::size_t detectedCount = worldFrame.objects.size();

    if (trackMaxDistance_ > 0.0) {
        std::unordered_set<veda::GlobalId> touchedGids;
        for (std::size_t i = 0; i < worldFrame.objects.size(); ++i) {
            auto& object = worldFrame.objects[i];
            const domain::WorldPoint rawPosition = object.pos;

            if (positionJitterRadius_ > 0.0) {
                const auto previous = byGid_.find(object.gid);
                if (previous != byGid_.end() && previous->second.cls == object.cls) {
                    object.pos = stabilizePosition(previous->second.pos, rawPosition, positionJitterRadius_);
                }
            }

            auto [entityIt, inserted] = byGid_.try_emplace(object.gid);
            TrackedEntity& entity = entityIt->second;
            if (inserted || !ambiguousMatches[i]) {
                if (!inserted && motionFrame > entity.lastMotionFrame && std::isfinite(entity.rawPos.x) &&
                    std::isfinite(entity.rawPos.y) && std::isfinite(rawPosition.x) && std::isfinite(rawPosition.y)) {
                    const double elapsedFrames = static_cast<double>(motionFrame - entity.lastMotionFrame);
                    entity.motionDelta.x = (rawPosition.x - entity.rawPos.x) / elapsedFrames;
                    entity.motionDelta.y = (rawPosition.y - entity.rawPos.y) / elapsedFrames;
                    entity.hasMotion = true;
                }
                entity.cls = object.cls;
                entity.rawPos = rawPosition;
                entity.pos = object.pos;
                entity.lastMotionFrame = motionFrame;
            }
            entity.missedWindows = 0;
            touchedGids.insert(object.gid);
            if (inserted || !ambiguousMatches[i]) {
                for (const auto& sourceId : fusedSourceIds[i]) {
                    if (sourceId.second != 0) {
                        idIndex_[sourceId] = object.gid;
                    }
                }
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
            if (byGid_.find(it->second) == byGid_.end()) {
                it = idIndex_.erase(it);
            } else {
                ++it;
            }
        }

        // 짧은 채널 인계/수신 공백에도 UI 객체가 사라지지 않도록,
        // 만료 전 트랙은 마지막 좌표로 유지한다.
        for (const auto& [gid, entity] : byGid_) {
            if (touchedGids.count(gid)) {
                continue;
            }

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
