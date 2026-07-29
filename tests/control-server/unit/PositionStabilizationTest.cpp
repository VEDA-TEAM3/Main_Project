/**
 * @file    PositionStabilizationTest.cpp
 * @brief   월드 좌표 공간 히스테리시스의 정지 잡음·이동 지연·gid 매칭 회귀 테스트
 */

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "fuse/GridFuser.h"
#include "metric/EuclideanMetric.h"

namespace {

std::vector<domain::ObservationFrame> makeSingleObjectFrame(veda::TimestampMs timestamp, double x, double y) {
    return {{timestamp, 0, {{1, veda::ObjectClass::Human, {x, y}}}}};
}

const domain::WorldObject& onlyObject(const domain::WorldFrame& frame) {
    EXPECT_EQ(frame.objects.size(), 1U);
    return frame.objects.front();
}

TEST(PositionStabilizationTest, HoldsNoiseInsideRadiusAndFollowsOutsideRadius) {
    auto metric = std::make_shared<EuclideanMetric>();
    GridFuser fuser(metric, 0.75, 2.0, 0.15);

    const auto first = fuser.fuse(makeSingleObjectFrame(100, 10.0, 5.0));
    const auto second = fuser.fuse(makeSingleObjectFrame(200, 10.10, 5.0));
    const auto third = fuser.fuse(makeSingleObjectFrame(300, 10.30, 5.0));
    const auto fourth = fuser.fuse(makeSingleObjectFrame(400, 10.16, 5.0));

    EXPECT_DOUBLE_EQ(onlyObject(first).pos.x, 10.0);
    EXPECT_DOUBLE_EQ(onlyObject(second).pos.x, 10.0);
    EXPECT_NEAR(onlyObject(third).pos.x, 10.15, 1e-12);
    EXPECT_NEAR(onlyObject(fourth).pos.x, 10.15, 1e-12);
    EXPECT_EQ(onlyObject(first).gid, onlyObject(fourth).gid);
}

TEST(PositionStabilizationTest, BoundsSpatialLagForMovingObject) {
    auto metric = std::make_shared<EuclideanMetric>();
    GridFuser fuser(metric, 0.75, 2.0, 0.15);

    double previousOutput = -1.0;
    for (int window = 0; window < 20; ++window) {
        const double rawX = static_cast<double>(window) * 0.2;
        const auto result = fuser.fuse(makeSingleObjectFrame(100 + window * 100, rawX, 0.0));
        const double outputX = onlyObject(result).pos.x;

        EXPECT_LE(std::abs(rawX - outputX), 0.15 + 1e-12);
        EXPECT_GE(outputX, previousOutput);
        previousOutput = outputX;
    }
}

TEST(PositionStabilizationTest, UsesRawPositionForTrackingDistance) {
    auto metric = std::make_shared<EuclideanMetric>();
    GridFuser fuser(metric, 0.75, 2.0, 0.15);

    const auto first = fuser.fuse(makeSingleObjectFrame(100, 0.0, 0.0));
    const auto second = fuser.fuse(makeSingleObjectFrame(200, 1.9, 0.0));
    const auto third = fuser.fuse(makeSingleObjectFrame(300, 3.8, 0.0));

    EXPECT_EQ(onlyObject(first).gid, onlyObject(second).gid);
    EXPECT_EQ(onlyObject(first).gid, onlyObject(third).gid);
    EXPECT_NEAR(onlyObject(second).pos.x, 1.75, 1e-12);
    EXPECT_NEAR(onlyObject(third).pos.x, 3.65, 1e-12);
}

TEST(PositionStabilizationTest, DisabledRadiusPreservesRawCoordinates) {
    auto metric = std::make_shared<EuclideanMetric>();
    GridFuser fuser(metric, 0.75, 2.0, 0.0);

    const auto first = fuser.fuse(makeSingleObjectFrame(100, 10.0, 5.0));
    const auto second = fuser.fuse(makeSingleObjectFrame(200, 10.10, 5.05));

    EXPECT_DOUBLE_EQ(onlyObject(first).pos.x, 10.0);
    EXPECT_DOUBLE_EQ(onlyObject(first).pos.y, 5.0);
    EXPECT_DOUBLE_EQ(onlyObject(second).pos.x, 10.10);
    EXPECT_DOUBLE_EQ(onlyObject(second).pos.y, 5.05);
}

}  // namespace
