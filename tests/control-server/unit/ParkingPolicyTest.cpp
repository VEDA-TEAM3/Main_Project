/**
 * @file ParkingPolicyTest.cpp
 * @brief StationaryParkingPolicy의 시간·이동·미관측 경계 회귀 테스트
 */

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include "interfaces/IParkingPolicy.h"
#include "parking/StationaryParkingPolicy.h"

namespace {
ParkingPolicyConfig makeConfig() {
    ParkingPolicyConfig config;
    config.stationaryDurationMs = 100;
    config.maxObservationGapMs = 200;
    config.movementToleranceM = 0.2;
    config.spaces = {{{{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}}}};
    return config;
}

domain::WorldFrame makeFrame(veda::TimestampMs timestamp, veda::ObjectClass cls, double x, double y,
                             bool observed = true) {
    domain::WorldObject object;
    object.gid = 1;
    object.cls = cls;
    object.pos = {x, y};
    if (observed) {
        object.sourceChannels.add(0);
    }

    domain::WorldFrame frame;
    frame.timestamp = timestamp;
    frame.objects.push_back(object);
    return frame;
}
}  // namespace

TEST(ParkingPolicyTest, HumanInsideParkingSpaceAlwaysPasses) {
    std::unique_ptr<IParkingPolicy> policy = std::make_unique<StationaryParkingPolicy>(makeConfig());

    auto frame = makeFrame(1000, veda::ObjectClass::Human, 1.0, 1.0);
    policy->apply(frame);
    frame.timestamp = 2000;
    policy->apply(frame);

    ASSERT_EQ(frame.objects.size(), 1U);
    EXPECT_EQ(frame.objects.front().cls, veda::ObjectClass::Human);
}

TEST(ParkingPolicyTest, StationaryVehicleIsSuppressedAfterDuration) {
    StationaryParkingPolicy policy(makeConfig());

    auto first = makeFrame(1000, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(first);
    ASSERT_EQ(first.objects.size(), 1U);

    auto second = makeFrame(1050, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(second);
    ASSERT_EQ(second.objects.size(), 1U);

    auto third = makeFrame(1100, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(third);
    EXPECT_TRUE(third.objects.empty());
}

TEST(ParkingPolicyTest, MovementResetsTimerAndRestoresParkedVehicle) {
    StationaryParkingPolicy policy(makeConfig());

    auto first = makeFrame(1000, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(first);
    auto second = makeFrame(1050, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(second);
    auto parked = makeFrame(1100, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(parked);
    ASSERT_TRUE(parked.objects.empty());

    auto moving = makeFrame(1150, veda::ObjectClass::Vehicle, 1.5, 1.0);
    policy.apply(moving);
    ASSERT_EQ(moving.objects.size(), 1U);
}

TEST(ParkingPolicyTest, CoastedFrameDoesNotResetStationaryStart) {
    StationaryParkingPolicy policy(makeConfig());

    auto first = makeFrame(1000, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(first);
    auto coasted = makeFrame(1050, veda::ObjectClass::Vehicle, 1.0, 1.0, false);
    policy.apply(coasted);
    ASSERT_EQ(coasted.objects.size(), 1U);

    auto observed = makeFrame(1100, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(observed);
    EXPECT_TRUE(observed.objects.empty());
}

TEST(ParkingPolicyTest, LongObservationGapResetsStationaryStart) {
    ParkingPolicyConfig config = makeConfig();
    config.maxObservationGapMs = 75;
    StationaryParkingPolicy policy(config);

    auto first = makeFrame(1000, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(first);
    auto afterGap = makeFrame(1100, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(afterGap);
    ASSERT_EQ(afterGap.objects.size(), 1U);

    auto second = makeFrame(1150, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(second);
    ASSERT_EQ(second.objects.size(), 1U);
    auto parked = makeFrame(1200, veda::ObjectClass::Vehicle, 1.0, 1.0);
    policy.apply(parked);
    EXPECT_TRUE(parked.objects.empty());
}

TEST(ParkingPolicyTest, RejectsDegenerateParkingPolygon) {
    ParkingPolicyConfig config = makeConfig();
    config.spaces.front().points = {{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}};

    EXPECT_THROW({ StationaryParkingPolicy policy(config); }, std::invalid_argument);
}
