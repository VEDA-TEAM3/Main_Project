/**
 * @file    FuserTest.cpp
 * @brief   Fuser 동일성·직접 동작·좌표 안정화를 한 파일에서 검증하는 격리 단위 테스트
 *
 * @details 작은 선형 경로, 96개 이상 그리드 경로, 추적/coasting, 비유한 입력을 고정 시드
 *          500윈도우와 명시 입력으로 검증한다.
 */

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "fuse/ConcatFuser.h"
#include "fuse/GridFuser.h"
#include "interfaces/IDistanceMetric.h"

namespace {

class TestDistanceMetric final : public IDistanceMetric {
public:
    double calculate(const domain::WorldPoint& lhs, const domain::WorldPoint& rhs) const override {
        return std::hypot(lhs.x - rhs.x, lhs.y - rhs.y);
    }
};

std::shared_ptr<IDistanceMetric> makeMetric() {
    return std::make_shared<TestDistanceMetric>();
}

bool sameDouble(double lhs, double rhs) {
    return std::bit_cast<std::uint64_t>(lhs) == std::bit_cast<std::uint64_t>(rhs);
}

std::string compareFrames(const domain::WorldFrame& expected, const domain::WorldFrame& actual) {
    if (expected.timestamp != actual.timestamp) {
        return "timestamp mismatch: expected=" + std::to_string(expected.timestamp) +
               " actual=" + std::to_string(actual.timestamp);
    }
    if (expected.level != actual.level) {
        return "frame risk level mismatch";
    }
    if (expected.objects.size() != actual.objects.size()) {
        return "object count mismatch: expected=" + std::to_string(expected.objects.size()) +
               " actual=" + std::to_string(actual.objects.size());
    }

    for (std::size_t index = 0; index < expected.objects.size(); ++index) {
        const auto& lhs = expected.objects[index];
        const auto& rhs = actual.objects[index];
        const std::string prefix = "object[" + std::to_string(index) + "] ";

        if (lhs.gid != rhs.gid) {
            return prefix + "gid mismatch";
        }
        if (lhs.cls != rhs.cls) {
            return prefix + "class mismatch";
        }
        if (!sameDouble(lhs.pos.x, rhs.pos.x) || !sameDouble(lhs.pos.y, rhs.pos.y)) {
            return prefix + "position mismatch";
        }
        if (lhs.riskLevel != rhs.riskLevel) {
            return prefix + "risk level mismatch";
        }
        if (lhs.nearestObj != rhs.nearestObj) {
            return prefix + "nearest object mismatch";
        }
        if (!sameDouble(lhs.nearestDist, rhs.nearestDist)) {
            return prefix + "nearest distance mismatch";
        }
        if (lhs.zoneId != rhs.zoneId) {
            return prefix + "zone mismatch";
        }
        if (lhs.sourceChannels.count != rhs.sourceChannels.count ||
            lhs.sourceChannels.truncated != rhs.sourceChannels.truncated) {
            return prefix + "source channel metadata mismatch";
        }
        for (std::uint8_t channel = 0; channel < lhs.sourceChannels.count; ++channel) {
            if (lhs.sourceChannels.ids[channel] != rhs.sourceChannels.ids[channel]) {
                return prefix + "source channel order mismatch";
            }
        }
    }
    return {};
}

std::vector<domain::ObservationFrame> makeOverlappingFrames(std::size_t totalObjects, int channelCount,
                                                            veda::TimestampMs timestamp = 1000) {
    std::vector<domain::ObservationFrame> frames(static_cast<std::size_t>(channelCount));
    for (int channel = 0; channel < channelCount; ++channel) {
        frames[static_cast<std::size_t>(channel)].ch = channel;
        frames[static_cast<std::size_t>(channel)].ts = timestamp + channel;
    }

    const std::size_t entities = totalObjects / static_cast<std::size_t>(channelCount);
    for (std::size_t entity = 0; entity < entities; ++entity) {
        const double baseX = static_cast<double>(entity % 32) * 3.0;
        const double baseY = static_cast<double>(entity / 32) * 3.0;
        for (int channel = 0; channel < channelCount; ++channel) {
            const double offset = static_cast<double>(channel) * 0.03;
            frames[static_cast<std::size_t>(channel)].objects.push_back(
                {static_cast<veda::ObjectId>(entity + 1),
                 (entity % 2 == 0) ? veda::ObjectClass::Human : veda::ObjectClass::Vehicle,
                 {baseX + offset, baseY - offset}});
        }
    }
    return frames;
}

std::vector<domain::ObservationFrame> makeRandomFrames(std::mt19937_64& random, veda::TimestampMs timestamp) {
    std::uniform_int_distribution<int> channelCountDistribution(1, 8);
    std::uniform_int_distribution<int> objectCountDistribution(0, 24);
    std::uniform_int_distribution<int> classDistribution(0, 2);
    std::uniform_real_distribution<double> coordinateDistribution(-75.0, 75.0);
    std::uniform_real_distribution<double> jitterDistribution(-0.35, 0.35);

    const int channelCount = channelCountDistribution(random);
    std::vector<domain::ObservationFrame> frames;
    frames.reserve(static_cast<std::size_t>(channelCount));
    for (int channel = 0; channel < channelCount; ++channel) {
        domain::ObservationFrame frame;
        frame.ch = channel;
        frame.ts = timestamp + channel;

        const int objectCount = objectCountDistribution(random);
        frame.objects.reserve(static_cast<std::size_t>(objectCount));
        for (int object = 0; object < objectCount; ++object) {
            const int sharedId = object % 8;
            const bool shared = object < 8;
            const double baseX =
                shared ? static_cast<double>(sharedId) * 4.0 : coordinateDistribution(random);
            const double baseY =
                shared ? static_cast<double>(sharedId % 3) * 5.0 : coordinateDistribution(random);
            const auto objectClass =
                classDistribution(random) == 0 ? veda::ObjectClass::Human : veda::ObjectClass::Vehicle;
            frame.objects.push_back(
                {static_cast<veda::ObjectId>(object + 1), objectClass,
                 {baseX + jitterDistribution(random), baseY + jitterDistribution(random)}});
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

void expectEquivalent(ConcatFuser& baseline, GridFuser& optimized,
                      const std::vector<domain::ObservationFrame>& frames) {
    const domain::WorldFrame expected = baseline.fuse(frames);
    const domain::WorldFrame actual = optimized.fuse(frames);
    EXPECT_TRUE(compareFrames(expected, actual).empty()) << compareFrames(expected, actual);
}

std::vector<domain::ObservationFrame> oneObject(veda::TimestampMs timestamp, veda::ChannelId channel,
                                                veda::ObjectId id, veda::ObjectClass objectClass, double x,
                                                double y) {
    return {{timestamp, channel, {{id, objectClass, {x, y}}}}};
}

TEST(FuserEquivalenceTest, PreservesEdgeCaseSemantics) {
    auto metric = makeMetric();
    ConcatFuser baseline(metric, 1.0, 2.0);
    GridFuser optimized(metric, 1.0, 2.0);

    expectEquivalent(baseline, optimized, {});

    std::vector<domain::ObservationFrame> frames = {
        {110, 0, {{1, veda::ObjectClass::Human, {-1.0, -1.0}},
                  {2, veda::ObjectClass::Human, {0.0, 0.0}}}},
        {100, 1, {{1, veda::ObjectClass::Human, {-1.0, 0.0}},
                  {2, veda::ObjectClass::Vehicle, {0.0, 0.0}}}},
        {120, 2, {{1, veda::ObjectClass::Human, {-1.0, -0.5}}}},
    };
    expectEquivalent(baseline, optimized, frames);

    // 동일 ObjectId 승계, 거리 fallback, 감지 누락 coasting, 5윈도우 이후 만료를 함께 검증한다.
    for (int window = 0; window < 8; ++window) {
        if (window == 1) {
            frames[0].objects[0].pos.x += 0.1;
            frames[1].objects[0].id = 99;
        }
        if (window >= 2)
            frames = {{120 + window, 0, {}}};
        expectEquivalent(baseline, optimized, frames);
    }
}

TEST(FuserEquivalenceTest, MatchesBaselineAcrossDeterministicRandomWindows) {
    auto metric = makeMetric();
    ConcatFuser baseline(metric, 0.75, 2.0);
    GridFuser optimized(metric, 0.75, 2.0);
    std::mt19937_64 random(0x56454441ULL);

    for (int window = 0; window < 500; ++window) {
        auto frames = makeRandomFrames(random, 10'000 + window * 100);
        const domain::WorldFrame expected = baseline.fuse(frames);
        const domain::WorldFrame actual = optimized.fuse(frames);
        const std::string difference = compareFrames(expected, actual);
        ASSERT_TRUE(difference.empty()) << "window=" << window << ": " << difference;
    }
}

TEST(FuserEquivalenceTest, PreservesNonFiniteAndExtremeInputBehavior) {
    auto metric = makeMetric();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const std::vector<domain::ObservationFrame> frames = {
        {100, 0, {{1, veda::ObjectClass::Human, {nan, 0.0}},
                  {2, veda::ObjectClass::Vehicle, {1.0e30, -1.0e30}}}},
        {101, 1, {{1, veda::ObjectClass::Human, {infinity, 0.0}},
                  {2, veda::ObjectClass::Vehicle, {1.0e30, -1.0e30}}}},
    };

    ConcatFuser baseline(metric, 1.0, 0.0);
    GridFuser optimized(metric, 1.0, 0.0);
    expectEquivalent(baseline, optimized, frames);

    ConcatFuser nanThresholdBaseline(metric, nan, 0.0);
    GridFuser nanThresholdOptimized(metric, nan, 0.0);
    expectEquivalent(nanThresholdBaseline, nanThresholdOptimized, frames);
}

TEST(GridFuserTest, EmptyInputReturnsDefaultFrame) {
    GridFuser fuser(makeMetric(), 1.0, 2.0);
    const auto result = fuser.fuse({});

    EXPECT_EQ(result.timestamp, 0);
    EXPECT_TRUE(result.objects.empty());
    EXPECT_EQ(result.level, veda::RiskLevel::None);
}

TEST(GridFuserTest, UsesMinimumTimestampAndAveragesMergedCoordinates) {
    GridFuser fuser(makeMetric(), 1.0, 2.0);
    const std::vector<domain::ObservationFrame> frames = {
        {120, 0, {{1, veda::ObjectClass::Human, {0.0, 0.0}}}},
        {100, 1, {{7, veda::ObjectClass::Human, {0.6, 0.8}}}},
    };

    const auto result = fuser.fuse(frames);

    ASSERT_EQ(result.objects.size(), 1U);
    EXPECT_EQ(result.timestamp, 100);
    EXPECT_DOUBLE_EQ(result.objects[0].pos.x, 0.3);
    EXPECT_DOUBLE_EQ(result.objects[0].pos.y, 0.4);
    EXPECT_EQ(result.objects[0].sourceChannels.count, 2);
    EXPECT_EQ(result.objects[0].sourceChannels.ids[0], 0);
    EXPECT_EQ(result.objects[0].sourceChannels.ids[1], 1);
}

TEST(GridFuserTest, KeepsSameChannelDifferentClassAndOutsideThresholdSeparate) {
    GridFuser fuser(makeMetric(), 1.0, 0.0);
    const std::vector<domain::ObservationFrame> frames = {
        {100, 0, {{1, veda::ObjectClass::Human, {0.0, 0.0}},
                  {2, veda::ObjectClass::Human, {0.1, 0.1}}}},
        {101, 1, {{3, veda::ObjectClass::Vehicle, {0.0, 0.0}},
                  {4, veda::ObjectClass::Human, {2.0, 0.0}}}},
    };

    const auto result = fuser.fuse(frames);

    EXPECT_EQ(result.objects.size(), 4U);
}

TEST(GridFuserTest, MergeThresholdIsInclusive) {
    GridFuser fuser(makeMetric(), 1.0, 0.0);
    const std::vector<domain::ObservationFrame> frames = {
        {100, 0, {{1, veda::ObjectClass::Human, {0.0, 0.0}}}},
        {101, 1, {{2, veda::ObjectClass::Human, {1.0, 0.0}}}},
    };

    const auto result = fuser.fuse(frames);

    ASSERT_EQ(result.objects.size(), 1U);
    EXPECT_DOUBLE_EQ(result.objects[0].pos.x, 0.5);
}

TEST(GridFuserTest, LargeInputExercisesGridPathAndPreservesExpectedClusters) {
    GridFuser fuser(makeMetric(), 0.75, 2.0);
    const auto frames = makeOverlappingFrames(128, 4);

    const auto result = fuser.fuse(frames);

    ASSERT_EQ(result.objects.size(), 32U);
    for (std::size_t entity = 0; entity < result.objects.size(); ++entity) {
        const auto& object = result.objects[entity];
        const double baseX = static_cast<double>(entity % 32) * 3.0;
        EXPECT_NEAR(object.pos.x, baseX + 0.045, 1e-12);
        EXPECT_NEAR(object.pos.y, -0.045, 1e-12);
        EXPECT_EQ(object.sourceChannels.count, 4);
    }
}

TEST(GridFuserTest, ExtremeLargeInputFallsBackWithoutCellConversionOverflow) {
    GridFuser fuser(makeMetric(), 0.75, 0.0);
    std::vector<domain::ObservationFrame> frames(1);
    frames[0].ts = 100;
    frames[0].ch = 0;
    for (int object = 0; object < 96; ++object) {
        frames[0].objects.push_back(
            {object + 1, veda::ObjectClass::Human,
             {1.0e30 + static_cast<double>(object), -1.0e30 - static_cast<double>(object)}});
    }

    const auto result = fuser.fuse(frames);

    EXPECT_EQ(result.objects.size(), 96U);
}

TEST(GridFuserTest, ChangedSourceIdUsesDistanceFallbackToPreserveGid) {
    GridFuser fuser(makeMetric(), 0.75, 2.0);
    const auto first = fuser.fuse(oneObject(100, 0, 1, veda::ObjectClass::Human, 0.0, 0.0));
    const auto second = fuser.fuse(oneObject(200, 0, 99, veda::ObjectClass::Human, 0.2, 0.0));

    ASSERT_EQ(first.objects.size(), 1U);
    ASSERT_EQ(second.objects.size(), 1U);
    EXPECT_EQ(first.objects[0].gid, second.objects[0].gid);
}

TEST(GridFuserTest, CoastsFiveMissedWindowsThenExpiresTrack) {
    GridFuser fuser(makeMetric(), 0.75, 2.0);
    const auto detected = fuser.fuse(oneObject(100, 0, 1, veda::ObjectClass::Human, 2.0, 3.0));
    ASSERT_EQ(detected.objects.size(), 1U);
    const auto gid = detected.objects[0].gid;

    for (int missed = 1; missed <= 5; ++missed) {
        const auto coasted = fuser.fuse({{100 + missed * 100, 0, {}}});
        ASSERT_EQ(coasted.objects.size(), 1U) << "missed=" << missed;
        EXPECT_EQ(coasted.objects[0].gid, gid);
        EXPECT_DOUBLE_EQ(coasted.objects[0].pos.x, 2.0);
        EXPECT_DOUBLE_EQ(coasted.objects[0].pos.y, 3.0);
    }

    const auto expired = fuser.fuse({{700, 0, {}}});
    EXPECT_TRUE(expired.objects.empty());
}

TEST(GridFuserTest, NonFiniteCoordinatesDoNotCrash) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    GridFuser fuser(makeMetric(), 1.0, 0.0);
    const std::vector<domain::ObservationFrame> frames = {
        {100, 0, {{1, veda::ObjectClass::Human, {nan, 0.0}}}},
        {101, 1, {{2, veda::ObjectClass::Human, {infinity, 0.0}}}},
    };

    EXPECT_NO_THROW({
        const auto result = fuser.fuse(frames);
        EXPECT_EQ(result.objects.size(), 1U);
        EXPECT_TRUE(std::isnan(result.objects[0].pos.x) || std::isinf(result.objects[0].pos.x));
    });
}

std::vector<domain::ObservationFrame> makeSingleObjectFrame(veda::TimestampMs timestamp, double x, double y) {
    return {{timestamp, 0, {{1, veda::ObjectClass::Human, {x, y}}}}};
}

const domain::WorldObject& onlyObject(const domain::WorldFrame& frame) {
    EXPECT_EQ(frame.objects.size(), 1U);
    return frame.objects.front();
}

TEST(PositionStabilizationTest, HoldsNoiseInsideRadiusAndFollowsOutsideRadius) {
    GridFuser fuser(makeMetric(), 0.75, 2.0, 0.15);

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
    GridFuser fuser(makeMetric(), 0.75, 2.0, 0.15);

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
    GridFuser fuser(makeMetric(), 0.75, 2.0, 0.15);

    const auto first = fuser.fuse(makeSingleObjectFrame(100, 0.0, 0.0));
    const auto second = fuser.fuse(makeSingleObjectFrame(200, 1.9, 0.0));
    const auto third = fuser.fuse(makeSingleObjectFrame(300, 3.8, 0.0));

    EXPECT_EQ(onlyObject(first).gid, onlyObject(second).gid);
    EXPECT_EQ(onlyObject(first).gid, onlyObject(third).gid);
    EXPECT_NEAR(onlyObject(second).pos.x, 1.75, 1e-12);
    EXPECT_NEAR(onlyObject(third).pos.x, 3.65, 1e-12);
}

TEST(PositionStabilizationTest, DisabledRadiusPreservesRawCoordinates) {
    GridFuser fuser(makeMetric(), 0.75, 2.0, 0.0);

    const auto first = fuser.fuse(makeSingleObjectFrame(100, 10.0, 5.0));
    const auto second = fuser.fuse(makeSingleObjectFrame(200, 10.10, 5.05));

    EXPECT_DOUBLE_EQ(onlyObject(first).pos.x, 10.0);
    EXPECT_DOUBLE_EQ(onlyObject(first).pos.y, 5.0);
    EXPECT_DOUBLE_EQ(onlyObject(second).pos.x, 10.10);
    EXPECT_DOUBLE_EQ(onlyObject(second).pos.y, 5.05);
}

}  // namespace
