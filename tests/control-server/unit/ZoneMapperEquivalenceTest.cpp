/**
 * @file    ZoneMapperEquivalenceTest.cpp
 * @brief   SpatialZoneMapper의 4구역/선형/결정표/폴백 경로와 무할당 회귀 테스트
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    bool directional = zones.size() == 4;
    std::array<bool, 4> seen{};
    for (const auto& zone : zones) {
        if (zone.zoneId < 0 || zone.zoneId >= 4 || seen[static_cast<std::size_t>(zone.zoneId)]) {
            directional = false;
            break;
        }
        seen[static_cast<std::size_t>(zone.zoneId)] = true;
    }

    for (auto& object : frame.objects) {
        object.zoneId = -1;
        if (directional && std::isfinite(object.pos.x) && std::isfinite(object.pos.y)) {
            const std::array<double, 4> scores = {object.pos.y, object.pos.x, -object.pos.y, -object.pos.x};
            object.zoneId =
                static_cast<veda::ChannelId>(std::max_element(scores.begin(), scores.end()) - scores.begin());
            continue;
        }
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
            return "object[" + std::to_string(index) +
                   "] zone mismatch: expected=" + std::to_string(expected.objects[index].zoneId) +
                   " actual=" + std::to_string(actual.objects[index].zoneId);
        }
    }
    return {};
}

}  // namespace zone_test

long g_zoneAllocCount = 0;
bool g_zoneAllocCounting = false;

void* operator new(std::size_t size) {
    if (g_zoneAllocCounting) {
        ++g_zoneAllocCount;
    }
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (g_zoneAllocCounting) {
        ++g_zoneAllocCount;
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

void expectEquivalent(const std::vector<SpatialZone>& zones, const domain::WorldFrame& input) {
    domain::WorldFrame expected = input;
    domain::WorldFrame actual = input;
    zone_test::referenceAssign(zones, expected);
    SpatialZoneMapper mapper(zones);
    mapper.assign(actual);
    EXPECT_TRUE(zone_test::compareZoneIds(expected, actual).empty()) << zone_test::compareZoneIds(expected, actual);
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
                                     (object % 2 == 0) ? veda::ObjectClass::Human : veda::ObjectClass::Vehicle,
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

TEST(ZoneMapperEquivalenceTest, FourDirectionalZonesUseHighestScoreAndLowerIdForTies) {
    const std::vector<SpatialZone> zones = {
        {2, -100.0, 100.0, 0.0, 100.0},
        {0, -100.0, 100.0, 0.0, -100.0},
        {3, -100.0, 0.0, -100.0, 100.0},
        {1, 0.0, 100.0, -100.0, 100.0},
    };
    domain::WorldFrame frame;
    frame.objects = {
        {1, veda::ObjectClass::Human, {0.0, 10.0}},  {2, veda::ObjectClass::Human, {10.0, 0.0}},
        {3, veda::ObjectClass::Human, {0.0, -10.0}}, {4, veda::ObjectClass::Human, {-10.0, 0.0}},
        {5, veda::ObjectClass::Human, {10.0, 10.0}},
    };

    SpatialZoneMapper mapper(zones);
    mapper.assign(frame);

    ASSERT_EQ(frame.objects.size(), 5U);
    EXPECT_EQ(frame.objects[0].zoneId, 0);
    EXPECT_EQ(frame.objects[1].zoneId, 1);
    EXPECT_EQ(frame.objects[2].zoneId, 2);
    EXPECT_EQ(frame.objects[3].zoneId, 3);
    EXPECT_EQ(frame.objects[4].zoneId, 0) << "45도 동점에서는 낮은 zoneId가 이겨야 함";
}

TEST(ZoneMapperEquivalenceTest, TwoFourChannelCctvsUseNearestCameraAndLocalDirection) {
    std::vector<SpatialZone> zones;
    std::vector<CameraCalibration> calibrations;
    for (int channel = 0; channel < 8; ++channel) {
        zones.push_back({channel, -100.0, 100.0, -100.0, 100.0});
        calibrations.push_back({channel, channel < 4 ? -50.0 : 50.0, 0.0, static_cast<double>(channel % 4) * 90.0, -1});
    }

    SpatialZoneMapper mapper(zones, 0.5, calibrations, true);
    domain::WorldFrame frame;
    frame.objects = {
        {1, veda::ObjectClass::Human, {-50.0, 10.0}},  {2, veda::ObjectClass::Human, {-40.0, 0.0}},
        {3, veda::ObjectClass::Human, {-50.0, -10.0}}, {4, veda::ObjectClass::Human, {-60.0, 0.0}},
        {5, veda::ObjectClass::Human, {50.0, 10.0}},   {6, veda::ObjectClass::Human, {60.0, 0.0}},
        {7, veda::ObjectClass::Human, {50.0, -10.0}},  {8, veda::ObjectClass::Human, {40.0, 0.0}},
        {9, veda::ObjectClass::Human, {0.0, 0.0}},
    };

    mapper.assign(frame);
    for (std::size_t index = 0; index < 8; ++index) {
        EXPECT_EQ(frame.objects[index].zoneId, static_cast<veda::ChannelId>(index));
    }
    EXPECT_EQ(frame.objects[8].zoneId, 1) << "CCTV 거리 동점에서는 낮은 zoneId 그룹이 이겨야 함";

    frame.objects = {{10, veda::ObjectClass::Human, {-1.0, 0.0}}};
    mapper.assign(frame);
    EXPECT_EQ(frame.objects[0].zoneId, 1);

    frame.objects[0].pos = {0.2, 0.0};
    mapper.assign(frame);
    EXPECT_EQ(frame.objects[0].zoneId, 1) << "0.5m 히스테리시스 안에서는 이전 CCTV를 유지해야 함";

    frame.objects[0].pos = {1.0, 0.0};
    mapper.assign(frame);
    EXPECT_EQ(frame.objects[0].zoneId, 7);
}

TEST(ZoneMapperEquivalenceTest, DirectionalHysteresisSurvivesShortObservationGap) {
    std::vector<SpatialZone> zones;
    std::vector<CameraCalibration> calibrations;
    for (int channel = 0; channel < 8; ++channel) {
        zones.push_back({channel, -100.0, 100.0, -100.0, 100.0});
        calibrations.push_back({channel, channel < 4 ? -50.0 : 50.0, 0.0, static_cast<double>(channel % 4) * 90.0, -1});
    }

    SpatialZoneMapper mapper(zones, 0.5, calibrations, true);
    domain::WorldFrame frame;
    frame.objects = {{10, veda::ObjectClass::Human, {-1.0, 0.0}}};
    mapper.assign(frame);
    ASSERT_EQ(frame.objects[0].zoneId, 1);

    frame.objects.clear();
    mapper.assign(frame);

    frame.objects = {{10, veda::ObjectClass::Human, {0.2, 0.0}}};
    mapper.assign(frame);
    EXPECT_EQ(frame.objects[0].zoneId, 1);
}

TEST(AppConfigTest, StartupValidationRequiresCompleteZonesAndCalibrations) {
    AppConfig config;
    config.channelCount = 8;
    config.directionalZoneMapping = true;
    for (int channel = 0; channel < config.channelCount; ++channel) {
        config.zones.push_back({channel, channel < 4 ? 0.0 : 12.785, channel < 4 ? 10.0 : 22.785, 0.0, 10.0});
        config.cameraCalibrations.push_back(
            {channel, channel < 4 ? -50.0 : 50.0, 0.0, static_cast<double>(channel % 4) * 90.0, -1});
    }

    EXPECT_NO_THROW(config.validateForStartup());

    config.cameraCalibrations.pop_back();
    EXPECT_THROW(config.validateForStartup(), std::invalid_argument);
    config.cameraCalibrations.push_back({7, 50.0, 0.0, 270.0, -1});
    config.zones.pop_back();
    EXPECT_THROW(config.validateForStartup(), std::invalid_argument);
}

TEST(AppConfigTest, DirectionalMappingRequiresOneSharedSquarePerCctv) {
    AppConfig config;
    config.channelCount = 8;
    config.directionalZoneMapping = true;
    for (int channel = 0; channel < config.channelCount; ++channel) {
        config.zones.push_back({channel, channel < 4 ? 0.0 : 12.785, channel < 4 ? 10.0 : 22.785, 0.0, 10.0});
        config.cameraCalibrations.push_back(
            {channel, channel < 4 ? 5.0 : 17.785, 5.0, static_cast<double>(channel % 4) * 90.0, -1});
    }

    EXPECT_NO_THROW(config.validateForStartup());

    config.zones[1].maxX = 9.0;
    EXPECT_THROW(config.validateForStartup(), std::invalid_argument);
}

TEST(ZoneMapperEquivalenceTest, DirectionalMappingDropsObjectsOutsideActiveCoverage) {
    std::vector<SpatialZone> zones;
    std::vector<CameraCalibration> calibrations;
    for (int channel = 0; channel < 8; ++channel) {
        zones.push_back({channel, channel < 4 ? 0.0 : 12.785, channel < 4 ? 10.0 : 22.785, 0.0, 10.0});
        calibrations.push_back({channel, channel < 4 ? 5.0 : 17.785, 5.0, static_cast<double>(channel % 4) * 90.0, -1});
    }

    SpatialZoneMapper mapper(zones, 0.7, calibrations, true);
    domain::WorldFrame frame;
    frame.objects = {
        {1, veda::ObjectClass::Human, {5.0, 7.5}},      {2, veda::ObjectClass::Human, {7.5, 5.0}},
        {3, veda::ObjectClass::Human, {5.0, 2.5}},      {4, veda::ObjectClass::Human, {2.5, 5.0}},
        {5, veda::ObjectClass::Human, {17.785, 7.5}},   {6, veda::ObjectClass::Human, {20.285, 5.0}},
        {7, veda::ObjectClass::Human, {17.785, 2.5}},   {8, veda::ObjectClass::Human, {15.285, 5.0}},
        {9, veda::ObjectClass::Human, {4.122, 11.498}}, {10, veda::ObjectClass::Human, {16.907, 11.498}},
        {11, veda::ObjectClass::Human, {11.0, 5.0}},
    };

    mapper.assign(frame);

    ASSERT_EQ(frame.objects.size(), 8U);
    for (std::size_t index = 0; index < frame.objects.size(); ++index) {
        EXPECT_EQ(frame.objects[index].zoneId, static_cast<veda::ChannelId>(index));
    }
}

TEST(ZoneMapperEquivalenceTest, DirectionalHysteresisKeepsPreviousZoneNearBoundary) {
    const std::vector<SpatialZone> zones = {
        {0, -100.0, 100.0, -100.0, 100.0},
        {1, -100.0, 100.0, -100.0, 100.0},
        {2, -100.0, 100.0, -100.0, 100.0},
        {3, -100.0, 100.0, -100.0, 100.0},
    };
    SpatialZoneMapper mapper(zones, 0.5);
    domain::WorldFrame frame;
    frame.objects = {{1, veda::ObjectClass::Human, {1.0, 2.0}}};

    mapper.assign(frame);
    EXPECT_EQ(frame.objects[0].zoneId, 0);

    frame.objects[0].pos = {1.1, 1.0};
    mapper.assign(frame);
    EXPECT_EQ(frame.objects[0].zoneId, 0);

    frame.objects[0].pos = {2.0, 1.0};
    mapper.assign(frame);
    EXPECT_EQ(frame.objects[0].zoneId, 1);
}

TEST(ZoneMapperEquivalenceTest, ConfigKeepsDirectionalZonesWhenLegacyBoundsAreInvalid) {
    const auto path = std::filesystem::temp_directory_path() / "veda_directional_zone_config.json";
    {
        std::ofstream file(path);
        file << R"({"channelCount":4,"zones":[
            {"zoneId":0,"minX":-100,"maxX":100,"minY":0,"maxY":-100},
            {"zoneId":1,"minX":0,"maxX":100,"minY":-100,"maxY":100},
            {"zoneId":2,"minX":-100,"maxX":100,"minY":0,"maxY":100},
            {"zoneId":3,"minX":-100,"maxX":0,"minY":-100,"maxY":100}]})";
    }

    const AppConfig config = AppConfig::load(path.string());
    std::filesystem::remove(path);

    ASSERT_EQ(config.zones.size(), 4U);
    EXPECT_EQ(config.zones[0].zoneId, 0);
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
