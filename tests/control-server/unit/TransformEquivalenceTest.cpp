/**
 * @file    TransformEquivalenceTest.cpp
 * @brief   최적화된 AffineLocalToWorldTransform의 수치·경계·버퍼 재사용 격리 테스트
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "Contract.h"
#include "Logger.h"
#include "core/AppConfig.h"
#include "domain/WorldObservation.h"
#include "transform/AffineLocalToWorldTransform.h"

namespace transform_test {

constexpr double kPi = 3.14159265358979323846;

std::vector<domain::ObservationFrame> referenceTransform(const std::vector<CameraCalibration>& calibrations,
                                                         bool dropUncalibrated, const WorldBounds& bounds,
                                                         const std::vector<veda::TopViewFrame>& input) {
    std::size_t maxChannel = 0;
    bool any = false;
    for (const auto& calibration : calibrations) {
        if (calibration.channelId < 0) {
            continue;
        }
        maxChannel = std::max(maxChannel, static_cast<std::size_t>(calibration.channelId));
        any = true;
    }

    std::vector<std::optional<CameraCalibration>> byChannel;
    if (any) {
        byChannel.resize(maxChannel + 1);
    }
    for (const auto& calibration : calibrations) {
        if (calibration.channelId < 0) {
            continue;
        }
        auto& slot = byChannel[static_cast<std::size_t>(calibration.channelId)];
        if (!slot.has_value()) {
            slot = calibration;
        }
    }

    std::vector<domain::ObservationFrame> output;
    output.reserve(input.size());
    for (const auto& frame : input) {
        const CameraCalibration* calibration = nullptr;
        if (frame.ch >= 0 && static_cast<std::size_t>(frame.ch) < byChannel.size()) {
            const auto& slot = byChannel[static_cast<std::size_t>(frame.ch)];
            if (slot.has_value()) {
                calibration = &slot.value();
            }
        }

        domain::ObservationFrame observed;
        observed.ts = frame.ts;
        observed.ch = frame.ch;
        if (calibration == nullptr) {
            if (!dropUncalibrated) {
                observed.objects.reserve(frame.objects.size());
                for (const auto& object : frame.objects) {
                    observed.objects.push_back({object.id, object.cls, domain::WorldPoint{object.pos.x, object.pos.y}});
                }
            }
            output.push_back(std::move(observed));
            continue;
        }

        const double thetaRad = (90.0 - calibration->facingAngleDeg) * kPi / 180.0;
        const double forwardX = std::cos(thetaRad);
        const double forwardY = std::sin(thetaRad);
        const double perpendicularX = (calibration->lateralSign > 0) ? forwardY : -forwardY;
        const double perpendicularY = (calibration->lateralSign > 0) ? -forwardX : forwardX;

        observed.objects.reserve(frame.objects.size());
        for (const auto& object : frame.objects) {
            const double worldX = calibration->cameraPosX + object.pos.x * perpendicularX + object.pos.y * forwardX;
            const double worldY = calibration->cameraPosY + object.pos.x * perpendicularY + object.pos.y * forwardY;
            if (!std::isfinite(worldX) || !std::isfinite(worldY)) {
                continue;
            }
            if (bounds.enabled &&
                (worldX < bounds.minX || worldX > bounds.maxX || worldY < bounds.minY || worldY > bounds.maxY)) {
                continue;
            }
            observed.objects.push_back({object.id, object.cls, domain::WorldPoint{worldX, worldY}});
        }
        output.push_back(std::move(observed));
    }
    return output;
}

bool sameDouble(double lhs, double rhs) {
    return std::bit_cast<std::uint64_t>(lhs) == std::bit_cast<std::uint64_t>(rhs);
}

std::string compareFrames(const std::vector<domain::ObservationFrame>& expected,
                          const std::vector<domain::ObservationFrame>& actual) {
    if (expected.size() != actual.size()) {
        return "frame count mismatch: expected=" + std::to_string(expected.size()) +
               " actual=" + std::to_string(actual.size());
    }
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        const auto& lhs = expected[frame];
        const auto& rhs = actual[frame];
        const std::string prefix = "frame[" + std::to_string(frame) + "] ";
        if (lhs.ts != rhs.ts) {
            return prefix + "timestamp mismatch";
        }
        if (lhs.ch != rhs.ch) {
            return prefix + "channel mismatch";
        }
        if (lhs.objects.size() != rhs.objects.size()) {
            return prefix + "object count mismatch";
        }
        for (std::size_t object = 0; object < lhs.objects.size(); ++object) {
            const auto& lhsObject = lhs.objects[object];
            const auto& rhsObject = rhs.objects[object];
            const std::string objectPrefix = prefix + "object[" + std::to_string(object) + "] ";
            if (lhsObject.id != rhsObject.id) {
                return objectPrefix + "id mismatch";
            }
            if (lhsObject.cls != rhsObject.cls) {
                return objectPrefix + "class mismatch";
            }
            if (!sameDouble(lhsObject.pos.x, rhsObject.pos.x) || !sameDouble(lhsObject.pos.y, rhsObject.pos.y)) {
                return objectPrefix + "position mismatch";
            }
        }
    }
    return {};
}

}  // namespace transform_test

long g_transformAllocCount = 0;
bool g_transformAllocCounting = false;

void* operator new(std::size_t size) {
    if (g_transformAllocCounting) {
        ++g_transformAllocCount;
    }
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (g_transformAllocCounting) {
        ++g_transformAllocCount;
    }
    return std::malloc(size != 0 ? size : 1);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }

void operator delete(void* memory) noexcept { std::free(memory); }

void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

[[maybe_unused]] const bool kLoggerDisabled = [] {
    LogConfig config;
    config.level = LogLevel::Off;
    config.console = false;
    config.file = false;
    initLogger(config);
    return true;
}();

std::vector<CameraCalibration> makeCalibrations(int channelCount) {
    std::vector<CameraCalibration> calibrations;
    calibrations.reserve(static_cast<std::size_t>(channelCount));
    for (int channel = 0; channel < channelCount; ++channel) {
        calibrations.push_back({channel, channel * 11.25 - 20.0, channel * -7.5 + 10.0, channel * 37.0 + 3.5,
                                (channel % 2 == 0) ? -1 : 1});
    }
    return calibrations;
}

TEST(TransformEquivalenceTest, PreservesCalibrationAndBoundsSemantics) {
    auto calibrations = makeCalibrations(4);
    calibrations.push_back({1, 999.0, 999.0, 270.0, -1});  // 중복 채널: 첫 항목이 이겨야 한다.
    calibrations.push_back({-1, 10.0, 10.0, 10.0, 1});     // 음수 채널: 무시해야 한다.
    const WorldBounds bounds{true, -30.0, 30.0, -25.0, 25.0};

    std::vector<veda::TopViewFrame> input = {
        {1,
         100,
         0,
         {{1, veda::ObjectClass::Human, {0.0, 0.0}, false}, {2, veda::ObjectClass::Vehicle, {5.0, 12.0}, true}}},
        {1, 101, 1, {{3, veda::ObjectClass::Human, {-3.0, 4.0}, false}}},
        {1, 102, 8, {{4, veda::ObjectClass::Vehicle, {1.0, 1.0}, false}}},
        {1, 103, -1, {{5, veda::ObjectClass::Human, {2.0, 2.0}, false}}},
    };

    AffineLocalToWorldTransform transform(calibrations, true, bounds);
    std::vector<domain::ObservationFrame> actual;
    transform.transform(input, actual);
    const auto expected = transform_test::referenceTransform(calibrations, true, bounds, input);
    EXPECT_TRUE(transform_test::compareFrames(expected, actual).empty())
        << transform_test::compareFrames(expected, actual);

    AffineLocalToWorldTransform passThrough(calibrations, false, bounds);
    passThrough.transform(input, actual);
    const auto passThroughExpected = transform_test::referenceTransform(calibrations, false, bounds, input);
    EXPECT_TRUE(transform_test::compareFrames(passThroughExpected, actual).empty())
        << transform_test::compareFrames(passThroughExpected, actual);
}

TEST(TransformEquivalenceTest, ReusesOutputWithoutLeavingObjectsFromPreviousCall) {
    const auto calibrations = makeCalibrations(4);
    AffineLocalToWorldTransform transform(calibrations, true, {});
    std::vector<domain::ObservationFrame> output(12);
    for (auto& frame : output) {
        frame.objects.resize(32);
    }

    for (int objectCount : {64, 1, 0, 17, 2}) {
        std::vector<veda::TopViewFrame> input(4);
        for (int channel = 0; channel < 4; ++channel) {
            input[static_cast<std::size_t>(channel)].ch = channel;
            input[static_cast<std::size_t>(channel)].ts = 1000 + objectCount;
            for (int object = 0; object < objectCount; ++object) {
                input[static_cast<std::size_t>(channel)].objects.push_back(
                    {object + 1,
                     veda::ObjectClass::Human,
                     {static_cast<double>(object) * 0.1, static_cast<double>(object) * -0.2},
                     false});
            }
        }
        transform.transform(input, output);
        const auto expected = transform_test::referenceTransform(calibrations, true, {}, input);
        ASSERT_TRUE(transform_test::compareFrames(expected, output).empty())
            << "objectCount=" << objectCount << ": " << transform_test::compareFrames(expected, output);
    }
}

TEST(TransformEquivalenceTest, MatchesReferenceAcrossDeterministicRandomFrames) {
    const auto calibrations = makeCalibrations(8);
    const WorldBounds bounds{true, -100.0, 100.0, -100.0, 100.0};
    AffineLocalToWorldTransform transform(calibrations, true, bounds);
    std::mt19937_64 random(0x5452414E53464F52ULL);
    std::uniform_int_distribution<int> frameCountDistribution(0, 12);
    std::uniform_int_distribution<int> channelDistribution(-1, 10);
    std::uniform_int_distribution<int> objectCountDistribution(0, 48);
    std::uniform_real_distribution<double> coordinateDistribution(-150.0, 150.0);
    std::vector<domain::ObservationFrame> output;

    for (int window = 0; window < 500; ++window) {
        std::vector<veda::TopViewFrame> input;
        const int frameCount = frameCountDistribution(random);
        input.reserve(static_cast<std::size_t>(frameCount));
        for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
            veda::TopViewFrame frame;
            frame.ts = 10'000 + window * 100 + frameIndex;
            frame.ch = channelDistribution(random);
            const int objectCount = objectCountDistribution(random);
            frame.objects.reserve(static_cast<std::size_t>(objectCount));
            for (int object = 0; object < objectCount; ++object) {
                frame.objects.push_back({object + 1,
                                         (object % 2 == 0) ? veda::ObjectClass::Human : veda::ObjectClass::Vehicle,
                                         {coordinateDistribution(random), coordinateDistribution(random)},
                                         object % 3 == 0});
            }
            input.push_back(std::move(frame));
        }

        transform.transform(input, output);
        const auto expected = transform_test::referenceTransform(calibrations, true, bounds, input);
        const std::string difference = transform_test::compareFrames(expected, output);
        ASSERT_TRUE(difference.empty()) << "window=" << window << ": " << difference;
    }
}

TEST(TransformEquivalenceTest, RejectsNonFiniteWorldCoordinatesWithAndWithoutBounds) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    auto calibrations = makeCalibrations(2);
    calibrations.push_back({2, 0.0, 0.0, nan, 1});
    const WorldBounds bounds{true, -10.0, 10.0, -10.0, 10.0};
    const std::vector<veda::TopViewFrame> input = {
        {1,
         100,
         0,
         {{1, veda::ObjectClass::Human, {nan, 1.0}, false},
          {2, veda::ObjectClass::Vehicle, {infinity, -infinity}, false},
          {4, veda::ObjectClass::Human, {1.0, 2.0}, false}}},
        {1, 101, 2, {{3, veda::ObjectClass::Human, {1.0, 2.0}, false}}},
    };

    AffineLocalToWorldTransform boundedTransform(calibrations, true, bounds);
    std::vector<domain::ObservationFrame> output;
    boundedTransform.transform(input, output);
    const auto boundedExpected = transform_test::referenceTransform(calibrations, true, bounds, input);
    EXPECT_TRUE(transform_test::compareFrames(boundedExpected, output).empty())
        << transform_test::compareFrames(boundedExpected, output);
    ASSERT_EQ(output.size(), 2U);
    EXPECT_TRUE(output[0].objects.empty());
    EXPECT_TRUE(output[1].objects.empty());

    AffineLocalToWorldTransform unboundedTransform(calibrations, true, {});
    unboundedTransform.transform(input, output);
    const auto unboundedExpected = transform_test::referenceTransform(calibrations, true, {}, input);
    EXPECT_TRUE(transform_test::compareFrames(unboundedExpected, output).empty())
        << transform_test::compareFrames(unboundedExpected, output);
    ASSERT_EQ(output.size(), 2U);
    ASSERT_EQ(output[0].objects.size(), 1U);
    EXPECT_EQ(output[0].objects[0].id, 4U);
    EXPECT_TRUE(output[1].objects.empty());
}

TEST(TransformEquivalenceTest, WarmPathReusesFrameAndObjectBuffersWithoutAllocation) {
    const auto calibrations = makeCalibrations(4);
    AffineLocalToWorldTransform transform(calibrations, true, {});
    std::vector<veda::TopViewFrame> input(4);
    for (int channel = 0; channel < 4; ++channel) {
        auto& frame = input[static_cast<std::size_t>(channel)];
        frame.ch = channel;
        frame.ts = 1000 + channel;
        frame.objects.reserve(64);
        for (int object = 0; object < 64; ++object) {
            frame.objects.push_back({object + 1,
                                     veda::ObjectClass::Human,
                                     {static_cast<double>(object), static_cast<double>(object) * 0.5},
                                     false});
        }
    }

    std::vector<domain::ObservationFrame> output;
    transform.transform(input, output);
    std::vector<std::size_t> capacities;
    capacities.reserve(output.size());
    for (const auto& frame : output) {
        capacities.push_back(frame.objects.capacity());
    }
    g_transformAllocCount = 0;
    g_transformAllocCounting = true;
    transform.transform(input, output);
    g_transformAllocCounting = false;

    EXPECT_EQ(g_transformAllocCount, 0) << "동일 크기 warm path는 기존 출력 버퍼를 재사용해야 함";
    ASSERT_EQ(output.size(), capacities.size());
    for (std::size_t index = 0; index < output.size(); ++index) {
        EXPECT_EQ(output[index].objects.capacity(), capacities[index]);
    }
}

}  // namespace
