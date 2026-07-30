/**
 * @file    ZoneMapperEquivalenceTest.cpp
 * @brief   SpatialZoneMapper의 4구역/선형/결정표/폴백 경로와 무할당 회귀 테스트
 */

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "Logger.h"
#include "core/AppConfig.h"
#include "domain/WorldFrame.h"
#include "zone/SpatialZoneMapper.h"

namespace zone_test {

void referenceAssign(const std::vector<SpatialZone>& zones, domain::WorldFrame& frame) {
    for (auto& object : frame.objects) {
        object.zoneId = -1;
        for (const auto& zone : zones) {
            if (object.pos.x >= zone.minX && object.pos.x <= zone.maxX && object.pos.y >= zone.minY &&
                object.pos.y <= zone.maxY) {
                object.zoneId = zone.zoneId;
                break;
            }
        }
    }
}

std::string compareZoneIds(const domain::WorldFrame& expected, const domain::WorldFrame& actual) {
    if (expected.objects.size() != actual.objects.size()) {
        return "object count mismatch: expected=" + std::to_string(expected.objects.size()) +
               " actual=" + std::to_string(actual.objects.size());
    }
    for (std::size_t index = 0; index < expected.objects.size(); ++index) {
        if (expected.objects[index].zoneId != actual.objects[index].zoneId) {
            return "object[" + std::to_string(index) + "] zone mismatch: expected=" +
                   std::to_string(expected.objects[index].zoneId) +
                   " actual=" + std::to_string(actual.objects[index].zoneId);
        }
    }
    return {};
}

}  // namespace zone_test

long g_zoneAllocCount = 0;
bool g_zoneAllocCounting = false;

void* operator new(std::size_t size) {
    if (g_zoneAllocCounting)
        ++g_zoneAllocCount;
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == nullptr)
        throw std::bad_alloc();
    return memory;
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

[[maybe_unused]] const bool kLoggerDisabled = [] {
    LogConfig config;
    config.level = LogLevel::Off;
    config.console = false;
    config.file = false;
    initLogger(config);
    return true;
}();

void expectEquivalent(const std::vector<SpatialZone>& zones, const domain::WorldFrame& input) {
    domain::WorldFrame expected = input;
    domain::WorldFrame actual = input;
    zone_test::referenceAssign(zones, expected);
    SpatialZoneMapper mapper(zones);
    mapper.assign(actual);
    EXPECT_TRUE(zone_test::compareZoneIds(expected, actual).empty())
        << zone_test::compareZoneIds(expected, actual);
}

TEST(ZoneMapperEquivalenceTest, PreservesFirstMatchAndInclusiveBoundaries) {
    const std::vector<SpatialZone> zones = {
        {10, -5.0, 5.0, -5.0, 5.0},
        {20, 0.0, 10.0, 0.0, 10.0},
        {-1, -1.0, 1.0, -1.0, 1.0},
    };
    domain::WorldFrame frame;
    frame.objects = {
        {1, veda::ObjectClass::Human, {-5.0, -5.0}},
        {2, veda::ObjectClass::Human, {5.0, 5.0}},
        {3, veda::ObjectClass::Human, {7.0, 7.0}},
        {4, veda::ObjectClass::Human, {100.0, 100.0}},
    };
    expectEquivalent(zones, frame);
}

TEST(ZoneMapperEquivalenceTest, PreservesInvalidAndNonFiniteInputBehavior) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const std::vector<SpatialZone> zones = {
        {1, 5.0, -5.0, -1.0, 1.0},
        {2, nan, 10.0, -10.0, 10.0},
        {3, -infinity, infinity, -infinity, infinity},
    };
    domain::WorldFrame frame;
    frame.objects = {
        {1, veda::ObjectClass::Human, {nan, 0.0}},
        {2, veda::ObjectClass::Human, {infinity, 0.0}},
        {3, veda::ObjectClass::Human, {-infinity, 0.0}},
        {4, veda::ObjectClass::Human, {0.0, 0.0}},
    };
    expectEquivalent(zones, frame);
}

TEST(ZoneMapperEquivalenceTest, IndexedPathPreservesOverlapBoundariesAndNonFiniteObjects) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<SpatialZone> zones = {
        {10, 0.0, 10.0, 0.0, 10.0},
        {20, 5.0, 15.0, 5.0, 15.0},
    };
    for (int zone = 2; zone < 64; ++zone) {
        const double offset = 1000.0 + zone * 20.0;
        zones.push_back({zone, offset, offset + 5.0, offset, offset + 5.0});
    }

    domain::WorldFrame frame;
    frame.objects = {
        {1, veda::ObjectClass::Human, {5.0, 5.0}},
        {2, veda::ObjectClass::Human, {10.0, 10.0}},
        {3, veda::ObjectClass::Human, {15.0, 15.0}},
        {4, veda::ObjectClass::Human, {nan, 5.0}},
    };
    expectEquivalent(zones, frame);
}

TEST(ZoneMapperEquivalenceTest, FallsBackWhenDecisionTableWouldExceedMemoryCap) {
    std::vector<SpatialZone> zones;
    for (int zone = 0; zone < 300; ++zone) {
        const double x = zone * 10.0;
        const double y = zone * 13.0;
        zones.push_back({zone, x, x + 1.0, y, y + 1.0});
    }
    domain::WorldFrame frame;
    frame.objects = {
        {1, veda::ObjectClass::Human, {0.5, 0.5}},
        {2, veda::ObjectClass::Human, {2990.5, 3887.5}},
        {3, veda::ObjectClass::Human, {-1.0, -1.0}},
    };
    expectEquivalent(zones, frame);
}

TEST(ZoneMapperEquivalenceTest, MatchesReferenceAcrossDeterministicRandomInputs) {
    std::mt19937_64 random(0x5A4F4E454D415050ULL);
    std::uniform_int_distribution<int> zoneCountDistribution(0, 80);
    std::uniform_int_distribution<int> objectCountDistribution(0, 300);
    std::uniform_real_distribution<double> coordinateDistribution(-250.0, 250.0);
    std::uniform_real_distribution<double> sizeDistribution(0.0, 40.0);

    for (int window = 0; window < 500; ++window) {
        std::vector<SpatialZone> zones;
        const int zoneCount = zoneCountDistribution(random);
        zones.reserve(static_cast<std::size_t>(zoneCount));
        for (int zone = 0; zone < zoneCount; ++zone) {
            const double minX = coordinateDistribution(random);
            const double minY = coordinateDistribution(random);
            zones.push_back({zone, minX, minX + sizeDistribution(random), minY, minY + sizeDistribution(random)});
        }

        domain::WorldFrame frame;
        const int objectCount = objectCountDistribution(random);
        frame.objects.reserve(static_cast<std::size_t>(objectCount));
        for (int object = 0; object < objectCount; ++object) {
            frame.objects.push_back({object + 1,
                                     (object % 2 == 0) ? veda::ObjectClass::Human
                                                       : veda::ObjectClass::Vehicle,
                                     {coordinateDistribution(random), coordinateDistribution(random)}});
        }

        domain::WorldFrame expected = frame;
        domain::WorldFrame actual = frame;
        zone_test::referenceAssign(zones, expected);
        SpatialZoneMapper mapper(zones);
        mapper.assign(actual);
        const std::string difference = zone_test::compareZoneIds(expected, actual);
        ASSERT_TRUE(difference.empty()) << "window=" << window << ": " << difference;
    }
}

TEST(ZoneMapperEquivalenceTest, FourZoneHotPathUsesInclusiveFirstMatchSemantics) {
    const std::vector<SpatialZone> zones = {
        {0, 0.0, 10.0, 0.0, 10.0},
        {1, 10.0, 20.0, 0.0, 10.0},
        {2, 0.0, 10.0, 10.0, 20.0},
        {3, 10.0, 20.0, 10.0, 20.0},
    };
    domain::WorldFrame frame;
    frame.objects = {
        {1, veda::ObjectClass::Human, {5.0, 5.0}},
        {2, veda::ObjectClass::Human, {10.0, 5.0}},
        {3, veda::ObjectClass::Human, {5.0, 15.0}},
        {4, veda::ObjectClass::Human, {15.0, 15.0}},
        {5, veda::ObjectClass::Human, {-1.0, -1.0}},
    };

    SpatialZoneMapper mapper(zones);
    mapper.assign(frame);

    ASSERT_EQ(frame.objects.size(), 5U);
    EXPECT_EQ(frame.objects[0].zoneId, 0);
    EXPECT_EQ(frame.objects[1].zoneId, 0) << "겹치는 경계에서는 먼저 선언된 zone이 이겨야 함";
    EXPECT_EQ(frame.objects[2].zoneId, 2);
    EXPECT_EQ(frame.objects[3].zoneId, 3);
    EXPECT_EQ(frame.objects[4].zoneId, -1);
}

TEST(ZoneMapperEquivalenceTest, WarmAssignPathDoesNotAllocate) {
    std::vector<SpatialZone> zones;
    zones.reserve(64);
    for (int zone = 0; zone < 64; ++zone) {
        const double x = static_cast<double>(zone % 8) * 10.0;
        const double y = static_cast<double>(zone / 8) * 10.0;
        zones.push_back({zone, x, x + 8.0, y, y + 8.0});
    }

    domain::WorldFrame frame;
    frame.objects.reserve(256);
    for (int object = 0; object < 256; ++object) {
        const int zone = object % 64;
        const double x = static_cast<double>(zone % 8) * 10.0 + 4.0;
        const double y = static_cast<double>(zone / 8) * 10.0 + 4.0;
        frame.objects.push_back({object + 1, veda::ObjectClass::Human, {x, y}});
    }

    SpatialZoneMapper mapper(zones);
    mapper.assign(frame);

    g_zoneAllocCount = 0;
    g_zoneAllocCounting = true;
    mapper.assign(frame);
    g_zoneAllocCounting = false;

    EXPECT_EQ(g_zoneAllocCount, 0) << "생성 후 assign warm path는 힙을 사용하지 않아야 함";
}

}  // namespace
