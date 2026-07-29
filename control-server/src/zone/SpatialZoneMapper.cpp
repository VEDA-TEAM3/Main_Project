#include "zone/SpatialZoneMapper.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "Logger.h"

#if defined(__GNUC__) || defined(__clang__)
#define VEDA_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define VEDA_ALWAYS_INLINE inline
#endif

namespace {
constexpr const char* kIface = "ZoneMapper";
constexpr std::size_t kMinIndexedZoneCount = 64;
constexpr std::size_t kMaxDecisionCells = 1'100'000;
constexpr std::uint32_t kNoZoneIndex = std::numeric_limits<std::uint32_t>::max();

bool contains(const SpatialZone& zone, const domain::WorldPoint& position) {
    return position.x >= zone.minX && position.x <= zone.maxX && position.y >= zone.minY &&
           position.y <= zone.maxY;
}

std::size_t axisBucket(const std::vector<double>& edges, double value) {
    const auto it = std::lower_bound(edges.begin(), edges.end(), value);
    const std::size_t edgeIndex = static_cast<std::size_t>(it - edges.begin());
    if (it != edges.end() && *it == value)
        return edgeIndex * 2 + 1;  // 경계값 자체
    return edgeIndex * 2;          // 경계 전/사이/후의 열린 구간
}
}  // namespace

SpatialZoneMapper::SpatialZoneMapper(std::vector<SpatialZone> zones) : zones_(std::move(zones)) {
    buildDecisionIndex();
}

void SpatialZoneMapper::buildDecisionIndex() {
    if (zones_.size() < kMinIndexedZoneCount ||
        zones_.size() >= static_cast<std::size_t>(kNoZoneIndex)) {
        return;
    }

    xEdges_.reserve(zones_.size() * 2);
    yEdges_.reserve(zones_.size() * 2);
    for (const auto& zone : zones_) {
        if (!std::isfinite(zone.minX) || !std::isfinite(zone.maxX) || !std::isfinite(zone.minY) ||
            !std::isfinite(zone.maxY) || zone.minX > zone.maxX || zone.minY > zone.maxY) {
            xEdges_.clear();
            yEdges_.clear();
            return;  // 예외 설정은 기존 선형 비교로 처리해 원래 동작을 보존한다.
        }
        xEdges_.push_back(zone.minX);
        xEdges_.push_back(zone.maxX);
        yEdges_.push_back(zone.minY);
        yEdges_.push_back(zone.maxY);
    }

    std::sort(xEdges_.begin(), xEdges_.end());
    xEdges_.erase(std::unique(xEdges_.begin(), xEdges_.end()), xEdges_.end());
    std::sort(yEdges_.begin(), yEdges_.end());
    yEdges_.erase(std::unique(yEdges_.begin(), yEdges_.end()), yEdges_.end());

    xBucketCount_ = xEdges_.size() * 2 + 1;
    const std::size_t yBucketCount = yEdges_.size() * 2 + 1;
    if (xBucketCount_ > kMaxDecisionCells / yBucketCount) {
        xEdges_.clear();
        yEdges_.clear();
        xBucketCount_ = 0;
        return;
    }

    winnerZoneIndices_.assign(xBucketCount_ * yBucketCount, kNoZoneIndex);
    // 뒤에서 앞으로 덮어써야 최종 셀에 가장 이른 선언 인덱스가 남는다.
    for (std::size_t reverse = zones_.size(); reverse > 0; --reverse) {
        const std::size_t zoneIndex = reverse - 1;
        const auto& zone = zones_[zoneIndex];
        const std::size_t xBegin = axisBucket(xEdges_, zone.minX);
        const std::size_t xEnd = axisBucket(xEdges_, zone.maxX);
        const std::size_t yBegin = axisBucket(yEdges_, zone.minY);
        const std::size_t yEnd = axisBucket(yEdges_, zone.maxY);
        for (std::size_t y = yBegin; y <= yEnd; ++y) {
            const std::size_t row = y * xBucketCount_;
            for (std::size_t x = xBegin; x <= xEnd; ++x)
                winnerZoneIndices_[row + x] = static_cast<std::uint32_t>(zoneIndex);
        }
    }
}

void SpatialZoneMapper::assign(domain::WorldFrame& frame) {
    if (winnerZoneIndices_.empty()) {
        assignLinear(frame);
        return;
    }
    assignIndexed(frame);
}

VEDA_ALWAYS_INLINE void SpatialZoneMapper::assignLinear(domain::WorldFrame& frame) const {
    if (zones_.size() == 4) {
        const auto& zone0 = zones_[0];
        const auto& zone1 = zones_[1];
        const auto& zone2 = zones_[2];
        const auto& zone3 = zones_[3];
        for (auto& obj : frame.objects) {
            obj.zoneId = -1;
            if (contains(zone0, obj.pos))
                obj.zoneId = zone0.zoneId;
            else if (contains(zone1, obj.pos))
                obj.zoneId = zone1.zoneId;
            else if (contains(zone2, obj.pos))
                obj.zoneId = zone2.zoneId;
            else if (contains(zone3, obj.pos))
                obj.zoneId = zone3.zoneId;

            if (obj.zoneId == -1) {
                logError(kIface, "gid=" + std::to_string(obj.gid) + " pos=(" + std::to_string(obj.pos.x) + ", " +
                                     std::to_string(obj.pos.y) +
                                     ") 이 어느 zone 상자에도 안 듦 — 미배정으로 남김");
            }
        }
        return;
    }

    for (auto& obj : frame.objects) {
        obj.zoneId = -1;
        for (const auto& zone : zones_) {
            if (obj.pos.x >= zone.minX && obj.pos.x <= zone.maxX && obj.pos.y >= zone.minY &&
                obj.pos.y <= zone.maxY) {
                obj.zoneId = zone.zoneId;
                break;
            }
        }
        if (obj.zoneId == -1) {
            logError(kIface, "gid=" + std::to_string(obj.gid) + " pos=(" + std::to_string(obj.pos.x) + ", " +
                                 std::to_string(obj.pos.y) + ") 이 어느 zone 상자에도 안 듦 — 미배정으로 남김");
        }
    }
}

#undef VEDA_ALWAYS_INLINE

void SpatialZoneMapper::assignIndexed(domain::WorldFrame& frame) const {
    for (auto& obj : frame.objects) {
        obj.zoneId = -1;

        if (std::isfinite(obj.pos.x) && std::isfinite(obj.pos.y)) {
            const std::size_t x = axisBucket(xEdges_, obj.pos.x);
            const std::size_t y = axisBucket(yEdges_, obj.pos.y);
            const std::uint32_t zoneIndex = winnerZoneIndices_[y * xBucketCount_ + x];
            if (zoneIndex != kNoZoneIndex)
                obj.zoneId = zones_[zoneIndex].zoneId;
        } else {
            for (const auto& zone : zones_) {
                if (contains(zone, obj.pos)) {
                    obj.zoneId = zone.zoneId;
                    break;
                }
            }
        }

        if (obj.zoneId == -1) {
            logError(kIface, "gid=" + std::to_string(obj.gid) + " pos=(" + std::to_string(obj.pos.x) + ", " +
                                 std::to_string(obj.pos.y) + ") 이 어느 zone 상자에도 안 듦 — 미배정으로 남김");
        }
    }
}
