#pragma once

#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include "core/AppConfig.h"
#include "domain/WorldFrame.h"

namespace zone_test {

inline void referenceAssign(const std::vector<SpatialZone>& zones, domain::WorldFrame& frame) {
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

inline std::string compareZoneIds(const domain::WorldFrame& expected, const domain::WorldFrame& actual) {
    if (expected.objects.size() != actual.objects.size()) {
        return "object count mismatch: expected=" + std::to_string(expected.objects.size()) +
               " actual=" + std::to_string(actual.objects.size());
    }
    for (std::size_t i = 0; i < expected.objects.size(); ++i) {
        if (expected.objects[i].zoneId != actual.objects[i].zoneId) {
            return "object[" + std::to_string(i) + "] zone mismatch: expected=" +
                   std::to_string(expected.objects[i].zoneId) +
                   " actual=" + std::to_string(actual.objects[i].zoneId);
        }
    }
    return {};
}

}  // namespace zone_test
