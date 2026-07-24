#include "zone/SpatialZoneMapper.h"

#include <string>

#include "Logger.h"

namespace {
constexpr const char* kIface = "ZoneMapper";
}  // namespace

SpatialZoneMapper::SpatialZoneMapper(std::vector<SpatialZone> zones) : zones_(std::move(zones)) {}

void SpatialZoneMapper::assign(domain::WorldFrame& frame) {
    for (auto& obj : frame.objects) {
        obj.zoneId = -1;

        // first-match wins: 선언 순서대로 검사, 처음 포함되는 상자로 배정 (경계 포함)
        for (const auto& z : zones_) {
            if (obj.pos.x >= z.minX && obj.pos.x <= z.maxX && obj.pos.y >= z.minY && obj.pos.y <= z.maxY) {
                obj.zoneId = z.zoneId;
                break;
            }
        }

        if (obj.zoneId == -1) {
            logError(kIface, "gid=" + std::to_string(obj.gid) + " pos=(" + std::to_string(obj.pos.x) + ", " +
                                 std::to_string(obj.pos.y) + ") 이 어느 zone 상자에도 안 듦 — 미배정으로 남김");
        }
    }
}
