#include "zone/SpatialZoneMapper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "Logger.h"

namespace {
constexpr const char* kIface = "ZoneMapper";
constexpr std::size_t kMinIndexedZoneCount = 64;
constexpr std::size_t kMaxDecisionCells = 1'100'000;
constexpr std::uint32_t kNoZoneIndex = std::numeric_limits<std::uint32_t>::max();
constexpr double kPi = 3.14159265358979323846;

bool contains(const SpatialZone& zone, const domain::WorldPoint& position) {
    return position.x >= zone.minX && position.x <= zone.maxX && position.y >= zone.minY && position.y <= zone.maxY;
}

/**
 * @brief zone 을 margin 만큼 넓힌 상자에 점이 드는가 (히스테리시스 판정)
 * @note  비유한 좌표는 모든 비교가 false 라 자연히 탈락한다 -- 별도 검사가 필요 없다.
 */
bool containsExpanded(const SpatialZone& zone, const domain::WorldPoint& position, double margin) {
    return position.x >= zone.minX - margin && position.x <= zone.maxX + margin && position.y >= zone.minY - margin &&
           position.y <= zone.maxY + margin;
}

std::size_t axisBucket(const std::vector<double>& edges, double value) {
    const auto it = std::lower_bound(edges.begin(), edges.end(), value);
    const std::size_t edgeIndex = static_cast<std::size_t>(it - edges.begin());
    if (it != edges.end() && *it == value) {
        return edgeIndex * 2 + 1;  // 경계값 자체
    }
    return edgeIndex * 2;  // 경계 전/사이/후의 열린 구간
}
}  // namespace

SpatialZoneMapper::SpatialZoneMapper(std::vector<SpatialZone> zones, double hysteresisMargin,
                                           std::vector<CameraCalibration> calibrations, bool directionalMode)
    : zones_(std::move(zones)), hysteresisMargin_(hysteresisMargin) {
    if (!std::isfinite(hysteresisMargin_) || hysteresisMargin_ < 0.0) {
        throw std::invalid_argument("zone hysteresis margin must be finite and non-negative");
    }

    bool legacyFourDirection = zones_.size() == 4;
    std::array<bool, 4> seen{};
    for (const SpatialZone& zone : zones_) {
        if (zone.zoneId < 0 || zone.zoneId >= 4 || seen[static_cast<std::size_t>(zone.zoneId)]) {
            legacyFourDirection = false;
            break;
        }
        seen[static_cast<std::size_t>(zone.zoneId)] = true;
    }

    const bool calibratedDirections = supportsDirectionalZoneMapping(zones_, calibrations);
    coverageFilterEnabled_ = directionalMode && calibratedDirections;
    directionalMode_ = (directionalMode || legacyFourDirection) && (calibratedDirections || legacyFourDirection);
    if (directionalMode_) {
        directionalZones_.resize(zones_.size());
        for (std::size_t zoneIndex = 0; zoneIndex < zones_.size(); ++zoneIndex) {
            double cameraX = 0.0;
            double cameraY = 0.0;
            double facingAngle = static_cast<double>(zones_[zoneIndex].zoneId % 4) * 90.0;
            if (calibratedDirections) {
                const auto calibration = std::find_if(
                    calibrations.begin(), calibrations.end(),
                    [this, zoneIndex](const CameraCalibration& candidate) {
                        return candidate.channelId == zones_[zoneIndex].zoneId;
                    });
                cameraX = calibration->cameraPosX;
                cameraY = calibration->cameraPosY;
                facingAngle = calibration->facingAngleDeg;
            }

            auto camera = std::find_if(cameraSites_.begin(), cameraSites_.end(),
                                       [cameraX, cameraY](const CameraSite& candidate) {
                                           return candidate.x == cameraX && candidate.y == cameraY;
                                       });
            if (camera == cameraSites_.end()) {
                cameraSites_.push_back(CameraSite{cameraX, cameraY, zones_[zoneIndex].zoneId});
                camera = cameraSites_.end() - 1;
            } else {
                camera->lowestZoneId = std::min(camera->lowestZoneId, zones_[zoneIndex].zoneId);
            }

            const double radians = facingAngle * kPi / 180.0;
            directionalZones_[zoneIndex] = {static_cast<std::uint32_t>(camera - cameraSites_.begin()),
                                             std::sin(radians), std::cos(radians)};
        }
    }
    buildDecisionIndex();
}

void SpatialZoneMapper::buildDecisionIndex() {
    if (directionalMode_ || zones_.size() < kMinIndexedZoneCount ||
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
            for (std::size_t x = xBegin; x <= xEnd; ++x) {
                winnerZoneIndices_[row + x] = static_cast<std::uint32_t>(zoneIndex);
            }
        }
    }
}

double SpatialZoneMapper::cameraDistance(std::uint32_t cameraIndex, const domain::WorldPoint& position) const {
    const CameraSite& camera = cameraSites_[cameraIndex];
    return std::hypot(position.x - camera.x, position.y - camera.y);
}

double SpatialZoneMapper::directionScore(std::uint32_t zoneIndex, const domain::WorldPoint& position) const {
    const DirectionalZone& direction = directionalZones_[zoneIndex];
    const CameraSite& camera = cameraSites_[direction.cameraIndex];
    return (position.x - camera.x) * direction.forwardX + (position.y - camera.y) * direction.forwardY;
}

std::uint32_t SpatialZoneMapper::nearestCamera(const domain::WorldPoint& position) const {
    std::uint32_t winner = 0;
    for (std::uint32_t camera = 1; camera < cameraSites_.size(); ++camera) {
        const double candidateDistance = cameraDistance(camera, position);
        const double winnerDistance = cameraDistance(winner, position);
        if (candidateDistance < winnerDistance ||
            (candidateDistance == winnerDistance &&
             cameraSites_[camera].lowestZoneId < cameraSites_[winner].lowestZoneId)) {
            winner = camera;
        }
    }
    return winner;
}

std::uint32_t SpatialZoneMapper::resolveDirectional(const domain::WorldPoint& position,
                                                     std::uint32_t cameraIndex) const {
    std::uint32_t winner = kNoZoneIndex;
    for (std::uint32_t zoneIndex = 0; zoneIndex < directionalZones_.size(); ++zoneIndex) {
        if (directionalZones_[zoneIndex].cameraIndex != cameraIndex) {
            continue;
        }
        if (winner == kNoZoneIndex || directionScore(zoneIndex, position) > directionScore(winner, position) ||
            (directionScore(zoneIndex, position) == directionScore(winner, position) &&
             zones_[zoneIndex].zoneId < zones_[winner].zoneId)) {
            winner = zoneIndex;
        }
    }
    return winner;
}

std::uint32_t SpatialZoneMapper::resolveLinear(const domain::WorldPoint& position) const {
    if (directionalMode_) {
        if (!std::isfinite(position.x) || !std::isfinite(position.y)) {
            return kNoZoneIndex;
        }
        return resolveDirectional(position, nearestCamera(position));
    }

    for (std::size_t i = 0; i < zones_.size(); ++i) {
        if (contains(zones_[i], position)) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return kNoZoneIndex;
}

std::uint32_t SpatialZoneMapper::resolveIndexed(const domain::WorldPoint& position) const {
    if (std::isfinite(position.x) && std::isfinite(position.y)) {
        const std::size_t x = axisBucket(xEdges_, position.x);
        const std::size_t y = axisBucket(yEdges_, position.y);
        return winnerZoneIndices_[y * xBucketCount_ + x];
    }
    // 비유한 좌표는 버킷 인덱싱이 불가능하므로 선형 비교로 넘긴다(어차피 전부 탈락한다)
    return resolveLinear(position);
}

std::uint32_t SpatialZoneMapper::findPrevZoneIndex(veda::GlobalId gid) const {
    const auto it = std::lower_bound(prevZones_.begin(), prevZones_.end(), gid,
                                     [](const GidZone& entry, veda::GlobalId key) { return entry.gid < key; });
    if (it == prevZones_.end() || it->gid != gid) {
        return kNoZoneIndex;
    }
    return it->zoneIndex;
}

void SpatialZoneMapper::assign(domain::WorldFrame& frame) {
    const bool indexed = !winnerZoneIndices_.empty();

    if (coverageFilterEnabled_) {
        std::erase_if(frame.objects, [this](const domain::WorldObject& object) {
            return std::none_of(zones_.begin(), zones_.end(), [&object](const SpatialZone& zone) {
                return contains(zone, object.pos);
            });
        });
    }

    currZones_.clear();                        // capacity 유지 -> warmup 이후 무할당
    currZones_.reserve(frame.objects.size());  // 이미 충분하면 no-op

    for (auto& obj : frame.objects) {
        std::uint32_t zoneIndex = kNoZoneIndex;

        // [1] 히스테리시스 -- 방향 점수 차이 또는 확장 AABB로 직전 zone을 유지.
        //
        // gid == 0 은 제외한다. ConcatFuser 가 초기값으로 0 을 넣으므로 서로 다른 실체가
        // 동시에 gid 0 을 달 수 있고, 그러면 한 객체의 직전 zone 이 다른 객체에 잘못
        // 적용된다. 안정된 식별자가 없는 객체에는 히스테리시스가 성립하지 않는다.
        if (hysteresisMargin_ > 0.0 && obj.gid != 0) {
            const std::uint32_t prevIndex = findPrevZoneIndex(obj.gid);
            if (prevIndex != kNoZoneIndex && prevIndex < zones_.size()) {
                if (directionalMode_) {
                    const std::uint32_t winnerIndex = resolveLinear(obj.pos);
                    if (winnerIndex != kNoZoneIndex) {
                        const std::uint32_t previousCamera = directionalZones_[prevIndex].cameraIndex;
                        const std::uint32_t winnerCamera = directionalZones_[winnerIndex].cameraIndex;
                        std::uint32_t selectedCamera = winnerCamera;
                        if (previousCamera != winnerCamera &&
                            cameraDistance(previousCamera, obj.pos) <=
                                cameraDistance(winnerCamera, obj.pos) + hysteresisMargin_) {
                            selectedCamera = previousCamera;
                        }

                        const std::uint32_t selectedWinner = resolveDirectional(obj.pos, selectedCamera);
                        if (previousCamera == selectedCamera &&
                            directionScore(prevIndex, obj.pos) + hysteresisMargin_ >=
                                directionScore(selectedWinner, obj.pos)) {
                            zoneIndex = prevIndex;
                        } else {
                            zoneIndex = selectedWinner;
                        }
                    }
                } else if (containsExpanded(zones_[prevIndex], obj.pos, hysteresisMargin_)) {
                    zoneIndex = prevIndex;
                }
            }
        }

        // [2] 유지되지 않았으면 방향 최고 점수 또는 AABB first-match로 판정
        if (zoneIndex == kNoZoneIndex) {
            zoneIndex = indexed ? resolveIndexed(obj.pos) : resolveLinear(obj.pos);
        }

        obj.zoneId = (zoneIndex != kNoZoneIndex) ? zones_[zoneIndex].zoneId : -1;

        if (obj.zoneId == -1) {
            logError(kIface, "gid=" + std::to_string(obj.gid) + " pos=(" + std::to_string(obj.pos.x) + ", " +
                                 std::to_string(obj.pos.y) + ") 을 어느 zone에도 배정할 수 없음 — 미배정으로 남김");
        }

        currZones_.push_back(GidZone{obj.gid, zoneIndex, 0});
    }

    // findPrevZoneIndex 가 이진 탐색을 쓰므로 gid 오름차순이어야 한다.
    // (융합 결과는 클러스터 순서라 gid 정렬을 보장하지 않는다)
    std::sort(currZones_.begin(), currZones_.end(), [](const GidZone& a, const GidZone& b) { return a.gid < b.gid; });

    // 현재 프레임에 없는 이력은 짧게 유지한다. 객체를 WorldFrame에 되살리지 않고 zone 선택
    // 이력만 보존하므로 Risk/Qt/HW에는 오래된 좌표가 전달되지 않는다.
    std::size_t retainedCount = 0;
    for (GidZone previous : prevZones_) {
        const auto observed = std::lower_bound(
            currZones_.begin(), currZones_.end(), previous.gid,
            [](const GidZone& entry, veda::GlobalId gid) { return entry.gid < gid; });
        if ((observed == currZones_.end() || observed->gid != previous.gid) &&
            previous.missedWindows < kMaxMissedWindows) {
            ++previous.missedWindows;
            prevZones_[retainedCount++] = previous;
        }
    }
    prevZones_.resize(retainedCount);
    currZones_.insert(currZones_.end(), prevZones_.begin(), prevZones_.end());
    std::sort(currZones_.begin(), currZones_.end(), [](const GidZone& a, const GidZone& b) { return a.gid < b.gid; });

    // swap 으로 세대 교체. 5개 윈도우를 넘긴 미관측 gid 는 자동 소거된다.
    // 두 벡터의 capacity 가 서로 오갈 뿐 해제는 일어나지 않는다.
    prevZones_.swap(currZones_);

    // swap 직후 currZones_ 는 '지난 세대의 빈 버퍼'라 capacity 가 0일 수 있다.
    // 여기서 미리 확보해 두지 않으면 다음 프레임의 reserve 가 할당을 일으켜,
    // 두 버퍼가 수렴하는 데 두 프레임이 걸린다(warmup 1회짜리 무할당 검증이 실패한다).
    currZones_.reserve(prevZones_.size());
}
