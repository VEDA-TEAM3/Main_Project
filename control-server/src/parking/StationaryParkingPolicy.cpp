#include "parking/StationaryParkingPolicy.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
constexpr double kBoundaryEpsilon = 1e-9;

bool pointOnSegment(const domain::WorldPoint& point, const ParkingPoint& a, const ParkingPoint& b) {
    const double cross = (point.y - a.y) * (b.x - a.x) - (point.x - a.x) * (b.y - a.y);
    if (std::abs(cross) > kBoundaryEpsilon) {
        return false;
    }

    return point.x >= std::min(a.x, b.x) - kBoundaryEpsilon &&
           point.x <= std::max(a.x, b.x) + kBoundaryEpsilon &&
           point.y >= std::min(a.y, b.y) - kBoundaryEpsilon &&
           point.y <= std::max(a.y, b.y) + kBoundaryEpsilon;
}

bool polygonContains(const ParkingSpace& space, const domain::WorldPoint& point) {
    bool inside = false;
    for (std::size_t i = 0, j = space.points.size() - 1; i < space.points.size(); j = i++) {
        const ParkingPoint& a = space.points[j];
        const ParkingPoint& b = space.points[i];
        if (pointOnSegment(point, a, b)) {
            return true;
        }

        const bool crosses = (a.y > point.y) != (b.y > point.y);
        if (crosses && point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x) {
            inside = !inside;
        }
    }
    return inside;
}
}  // namespace

StationaryParkingPolicy::StationaryParkingPolicy(ParkingPolicyConfig config)
    : spaces_(std::move(config.spaces)),
      stationaryDurationMs_(config.stationaryDurationMs),
      maxObservationGapMs_(config.maxObservationGapMs),
      movementToleranceSquared_(config.movementToleranceM * config.movementToleranceM) {
    if (stationaryDurationMs_ == 0) {
        throw std::invalid_argument("parking stationaryDurationMs must be greater than zero");
    }
    if (maxObservationGapMs_ == 0) {
        throw std::invalid_argument("parking maxObservationGapMs must be greater than zero");
    }
    if (!std::isfinite(config.movementToleranceM) || config.movementToleranceM < 0.0) {
        throw std::invalid_argument("parking movementToleranceM must be finite and non-negative");
    }
    if (spaces_.size() > kMaxParkingSpaces) {
        throw std::invalid_argument("too many parking spaces");
    }
    for (const ParkingSpace& space : spaces_) {
        if (!isValidParkingSpace(space)) {
            throw std::invalid_argument("parking space must be a finite, non-degenerate polygon");
        }
    }
}

bool StationaryParkingPolicy::contains(const domain::WorldPoint& point) const {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        return false;
    }
    return std::any_of(spaces_.begin(), spaces_.end(),
                       [&point](const ParkingSpace& space) { return polygonContains(space, point); });
}

StationaryParkingPolicy::VehicleState* StationaryParkingPolicy::findState(veda::GlobalId gid) {
    const auto it = std::find_if(states_.begin(), states_.end(),
                                 [gid](const VehicleState& state) { return state.gid == gid; });
    return it == states_.end() ? nullptr : &*it;
}

bool StationaryParkingPolicy::shouldSuppress(const domain::WorldObject& object, veda::TimestampMs timestamp) {
    if (object.cls != veda::ObjectClass::Vehicle || object.gid <= 0 || timestamp <= 0 || !contains(object.pos)) {
        return false;
    }

    VehicleState* state = findState(object.gid);

    // GridFuser의 coast 객체는 관측 증거가 아니므로 시간 기준을 갱신하지 않는다.
    // 이미 주차 판정된 차량만 계속 숨겨 일시적인 재표시를 막는다.
    if (object.sourceChannels.empty()) {
        if (state != nullptr) {
            state->seen = true;
            return state->parked;
        }
        return false;
    }

    if (state == nullptr) {
        states_.push_back({object.gid, object.pos, timestamp, timestamp, false, true});
        return false;
    }

    state->seen = true;
    const double dx = object.pos.x - state->anchor.x;
    const double dy = object.pos.y - state->anchor.y;
    const bool moved = dx * dx + dy * dy > movementToleranceSquared_;
    const bool timestampReversed = timestamp < state->lastObservedTimestamp;
    const std::uint64_t observationGap =
        timestampReversed ? 0 : static_cast<std::uint64_t>(timestamp - state->lastObservedTimestamp);
    if (moved || timestampReversed || observationGap > maxObservationGapMs_) {
        state->anchor = object.pos;
        state->stationarySince = timestamp;
        state->lastObservedTimestamp = timestamp;
        state->parked = false;
        return false;
    }

    state->lastObservedTimestamp = timestamp;
    state->parked = static_cast<std::uint64_t>(timestamp - state->stationarySince) >= stationaryDurationMs_;
    return state->parked;
}

void StationaryParkingPolicy::apply(domain::WorldFrame& frame) {
    if (spaces_.empty()) {
        return;
    }

    for (VehicleState& state : states_) {
        state.seen = false;
    }

    std::erase_if(frame.objects,
                  [this, timestamp = frame.timestamp](const domain::WorldObject& object) {
                      return shouldSuppress(object, timestamp);
                  });

    std::erase_if(states_, [](const VehicleState& state) { return !state.seen; });
}
