#pragma once

/**
 * @file    StationaryParkingPolicy.h
 * @brief   주차면 안에서 일정 시간 정지한 차량을 제외하는 정책
 */

#include <cstdint>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/IParkingPolicy.h"

class StationaryParkingPolicy final : public IParkingPolicy {
public:
    /** @param config 주차면 및 정지 판정 설정 */
    explicit StationaryParkingPolicy(ParkingPolicyConfig config);
    ~StationaryParkingPolicy() override = default;

    void apply(domain::WorldFrame& frame) override;

private:
    struct VehicleState {
        veda::GlobalId gid = 0;
        domain::WorldPoint anchor;
        veda::TimestampMs stationarySince = 0;
        veda::TimestampMs lastObservedTimestamp = 0;
        bool parked = false;
        bool seen = false;
    };

    bool contains(const domain::WorldPoint& point) const;
    VehicleState* findState(veda::GlobalId gid);
    bool shouldSuppress(const domain::WorldObject& object, veda::TimestampMs timestamp);

    std::vector<ParkingSpace> spaces_;
    std::vector<VehicleState> states_;
    std::uint64_t stationaryDurationMs_ = 0;
    std::uint64_t maxObservationGapMs_ = 0;
    double movementToleranceSquared_ = 0.0;
};
