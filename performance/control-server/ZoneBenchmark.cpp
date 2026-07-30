#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

#include "core/AppConfig.h"
#include "domain/WorldFrame.h"
#include "zone/SpatialZoneMapper.h"

namespace {

using Clock = std::chrono::steady_clock;

std::vector<SpatialZone> makeZones(int zoneCount) {
    int columns = 1;
    while (columns * columns < zoneCount) {
        ++columns;
    }

    std::vector<SpatialZone> zones;
    zones.reserve(static_cast<std::size_t>(zoneCount));
    for (int zone = 0; zone < zoneCount; ++zone) {
        const int column = zone % columns;
        const int row = zone / columns;
        const double minX = column * 10.0;
        const double minY = row * 10.0;
        zones.push_back({zone, minX, minX + 8.0, minY, minY + 8.0});
    }
    return zones;
}

domain::WorldFrame makeFrame(const std::vector<SpatialZone>& zones, int objectCount) {
    domain::WorldFrame frame;
    frame.objects.reserve(static_cast<std::size_t>(objectCount));
    for (int object = 0; object < objectCount; ++object) {
        const auto& zone = zones[static_cast<std::size_t>(object) % zones.size()];
        frame.objects.push_back(
            {object + 1, (object % 2 == 0) ? veda::ObjectClass::Human : veda::ObjectClass::Vehicle,
             {(zone.minX + zone.maxX) * 0.5, (zone.minY + zone.maxY) * 0.5}});
    }
    return frame;
}

double measureMedianNsPerCall(int zoneCount, int objectCount, int repetitions) {
    constexpr int kTrials = 9;
    std::vector<double> samples;
    samples.reserve(kTrials);
    const auto zones = makeZones(zoneCount);

    for (int trial = 0; trial < kTrials; ++trial) {
        SpatialZoneMapper mapper(zones);
        domain::WorldFrame frame = makeFrame(zones, objectCount);
        for (int warmup = 0; warmup < 50; ++warmup) {
            mapper.assign(frame);
        }

        const auto begin = Clock::now();
        for (int repetition = 0; repetition < repetitions; ++repetition) {
            mapper.assign(frame);
        }
        const auto end = Clock::now();

        std::int64_t checksum = 0;
        for (const auto& object : frame.objects) {
            checksum += object.zoneId;
        }
        if (checksum < 0) {
            std::abort();
        }

        const double elapsedNs =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        samples.push_back(elapsedNs / static_cast<double>(repetitions));
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

}  // namespace

int main() {
    struct Case {
        int zones;
        int objects;
        int repetitions;
    };
    const std::vector<Case> cases = {
        {4, 16, 100'000},
        {4, 64, 40'000},
        {4, 256, 12'000},
        {4, 1024, 3'000},
        {16, 1024, 1'500},
        {64, 1024, 500},
        {256, 1024, 150},
    };

    std::cout << "zones,objects,ns_per_call\n";
    std::cout << std::fixed << std::setprecision(2);
    for (const auto& benchmark : cases) {
        std::cout << benchmark.zones << ',' << benchmark.objects << ','
                  << measureMedianNsPerCall(benchmark.zones, benchmark.objects, benchmark.repetitions) << '\n';
    }
}
