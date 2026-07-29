/**
 * @file    FuserEquivalenceTest.cpp
 * @brief   ConcatFuser 기준 구현과 최적화 GridFuser의 비트 단위 회귀 비교
 *
 * @details 작은 선형 경로, 96개 이상 그리드 경로, 추적/coasting, 비유한 입력을 고정 시드
 *          500윈도우와 명시 입력으로 검증한다.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <limits>
#include <random>
#include <vector>

#include "FuserTestSupport.h"
#include "fuse/ConcatFuser.h"
#include "fuse/GridFuser.h"
#include "metric/EuclideanMetric.h"

namespace {

void expectEquivalent(ConcatFuser& baseline, GridFuser& optimized,
                      const std::vector<domain::ObservationFrame>& frames) {
    const domain::WorldFrame expected = baseline.fuse(frames);
    const domain::WorldFrame actual = optimized.fuse(frames);
    EXPECT_TRUE(fuser_test::compareFrames(expected, actual).empty())
        << fuser_test::compareFrames(expected, actual);
}

std::shared_ptr<EuclideanMetric> makeMetric() {
    return std::make_shared<EuclideanMetric>();
}

std::vector<domain::ObservationFrame> oneObject(veda::TimestampMs timestamp, veda::ChannelId channel,
                                                veda::ObjectId id, veda::ObjectClass objectClass, double x,
                                                double y) {
    return {{timestamp, channel, {{id, objectClass, {x, y}}}}};
}

TEST(FuserEquivalenceTest, PreservesEdgeCaseSemantics) {
    auto metric = std::make_shared<EuclideanMetric>();
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
    auto metric = std::make_shared<EuclideanMetric>();
    ConcatFuser baseline(metric, 0.75, 2.0);
    GridFuser optimized(metric, 0.75, 2.0);
    std::mt19937_64 random(0x56454441ULL);

    for (int window = 0; window < 500; ++window) {
        auto frames = fuser_test::makeRandomFrames(random, 10'000 + window * 100);
        const domain::WorldFrame expected = baseline.fuse(frames);
        const domain::WorldFrame actual = optimized.fuse(frames);
        const std::string difference = fuser_test::compareFrames(expected, actual);
        ASSERT_TRUE(difference.empty()) << "window=" << window << ": " << difference;
    }
}

TEST(FuserEquivalenceTest, PreservesNonFiniteAndExtremeInputBehavior) {
    auto metric = std::make_shared<EuclideanMetric>();
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
    const auto frames = fuser_test::makeOverlappingFrames(128, 4);

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

}  // namespace
