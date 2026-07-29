#pragma once

#include <bit>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "domain/WorldFrame.h"
#include "domain/WorldObservation.h"

namespace fuser_test {

inline bool sameDouble(double lhs, double rhs) {
    return std::bit_cast<std::uint64_t>(lhs) == std::bit_cast<std::uint64_t>(rhs);
}

inline std::string compareFrames(const domain::WorldFrame& expected, const domain::WorldFrame& actual) {
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

    for (std::size_t i = 0; i < expected.objects.size(); ++i) {
        const auto& lhs = expected.objects[i];
        const auto& rhs = actual.objects[i];
        const std::string prefix = "object[" + std::to_string(i) + "] ";

        if (lhs.gid != rhs.gid)
            return prefix + "gid mismatch";
        if (lhs.cls != rhs.cls)
            return prefix + "class mismatch";
        if (!sameDouble(lhs.pos.x, rhs.pos.x) || !sameDouble(lhs.pos.y, rhs.pos.y))
            return prefix + "position mismatch";
        if (lhs.riskLevel != rhs.riskLevel)
            return prefix + "risk level mismatch";
        if (lhs.nearestObj != rhs.nearestObj)
            return prefix + "nearest object mismatch";
        if (!sameDouble(lhs.nearestDist, rhs.nearestDist))
            return prefix + "nearest distance mismatch";
        if (lhs.zoneId != rhs.zoneId)
            return prefix + "zone mismatch";
        if (lhs.sourceChannels.count != rhs.sourceChannels.count ||
            lhs.sourceChannels.truncated != rhs.sourceChannels.truncated) {
            return prefix + "source channel metadata mismatch";
        }
        for (std::uint8_t channel = 0; channel < lhs.sourceChannels.count; ++channel) {
            if (lhs.sourceChannels.ids[channel] != rhs.sourceChannels.ids[channel])
                return prefix + "source channel order mismatch";
        }
    }
    return {};
}

inline std::vector<domain::ObservationFrame> makeOverlappingFrames(std::size_t totalObjects, int channelCount,
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

inline std::vector<domain::ObservationFrame> makeRandomFrames(std::mt19937_64& random,
                                                              veda::TimestampMs timestamp) {
    std::uniform_int_distribution<int> channelCountDist(1, 8);
    std::uniform_int_distribution<int> objectCountDist(0, 24);
    std::uniform_int_distribution<int> classDist(0, 2);
    std::uniform_real_distribution<double> coordinateDist(-75.0, 75.0);
    std::uniform_real_distribution<double> jitterDist(-0.35, 0.35);

    const int channelCount = channelCountDist(random);
    std::vector<domain::ObservationFrame> frames;
    frames.reserve(static_cast<std::size_t>(channelCount));
    for (int channel = 0; channel < channelCount; ++channel) {
        domain::ObservationFrame frame;
        frame.ch = channel;
        frame.ts = timestamp + channel;

        const int objectCount = objectCountDist(random);
        frame.objects.reserve(static_cast<std::size_t>(objectCount));
        for (int object = 0; object < objectCount; ++object) {
            const int sharedId = object % 8;
            const bool shared = object < 8;
            const double baseX = shared ? static_cast<double>(sharedId) * 4.0 : coordinateDist(random);
            const double baseY = shared ? static_cast<double>(sharedId % 3) * 5.0 : coordinateDist(random);
            const auto cls = classDist(random) == 0 ? veda::ObjectClass::Human : veda::ObjectClass::Vehicle;
            frame.objects.push_back(
                {static_cast<veda::ObjectId>(object + 1), cls,
                 {baseX + jitterDist(random), baseY + jitterDist(random)}});
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

}  // namespace fuser_test
