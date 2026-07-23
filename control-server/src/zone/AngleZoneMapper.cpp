#include "zone/AngleZoneMapper.h"

#include <cmath>
#include <string>

#include "Logger.h"

namespace {

constexpr const char* kIface = "ZoneMapper";
constexpr double kPi = 3.14159265358979323846;

double normalizeDeg(double deg) {
    while (deg < 0.0)
        deg += 360.0;
    while (deg >= 360.0)
        deg -= 360.0;
    return deg;
}

bool inRange(double angle, double minDeg, double maxDeg) {
    if (minDeg <= maxDeg) {
        return angle >= minDeg && angle < maxDeg;
    }
    return angle >= minDeg || angle < maxDeg;
}

}  // namespace

AngleZoneMapper::AngleZoneMapper(std::vector<ZoneBoundary> boundaries) : boundaries_(std::move(boundaries)) {}

void AngleZoneMapper::assign(domain::WorldFrame& frame) {
    for (auto& obj : frame.objects) {
        double angleDeg = normalizeDeg(std::atan2(obj.pos.y, obj.pos.x) * 180.0 / kPi);

        obj.zoneId = -1;
        for (const auto& b : boundaries_) {
            if (inRange(angleDeg, b.angleMinDeg, b.angleMaxDeg)) {
                obj.zoneId = b.channelId;
                break;
            }
        }

        if (obj.zoneId == -1) {
            logError(kIface, "gid=" + std::to_string(obj.gid) + " angle=" + std::to_string(angleDeg) +
                                 "deg 에 해당하는 zoneBoundary 없음 — 미배정으로 남김");
        }
    }
}