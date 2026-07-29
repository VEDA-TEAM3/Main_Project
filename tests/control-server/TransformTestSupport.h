#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Contract.h"
#include "core/AppConfig.h"
#include "domain/WorldObservation.h"

namespace transform_test {

inline constexpr double kPi = 3.14159265358979323846;

inline std::vector<domain::ObservationFrame> referenceTransform(
    const std::vector<CameraCalibration>& calibrations, bool dropUncalibrated, const WorldBounds& bounds,
    const std::vector<veda::TopViewFrame>& input) {
    std::size_t maxChannel = 0;
    bool any = false;
    for (const auto& calibration : calibrations) {
        if (calibration.channelId < 0)
            continue;
        maxChannel = std::max(maxChannel, static_cast<std::size_t>(calibration.channelId));
        any = true;
    }

    std::vector<std::optional<CameraCalibration>> byChannel;
    if (any)
        byChannel.resize(maxChannel + 1);
    for (const auto& calibration : calibrations) {
        if (calibration.channelId < 0)
            continue;
        auto& slot = byChannel[static_cast<std::size_t>(calibration.channelId)];
        if (!slot.has_value())
            slot = calibration;
    }

    std::vector<domain::ObservationFrame> output;
    output.reserve(input.size());
    for (const auto& frame : input) {
        const CameraCalibration* calibration = nullptr;
        if (frame.ch >= 0 && static_cast<std::size_t>(frame.ch) < byChannel.size()) {
            const auto& slot = byChannel[static_cast<std::size_t>(frame.ch)];
            if (slot.has_value())
                calibration = &slot.value();
        }

        domain::ObservationFrame observed;
        observed.ts = frame.ts;
        observed.ch = frame.ch;
        if (calibration == nullptr) {
            if (!dropUncalibrated) {
                observed.objects.reserve(frame.objects.size());
                for (const auto& object : frame.objects) {
                    observed.objects.push_back(
                        {object.id, object.cls, domain::WorldPoint{object.pos.x, object.pos.y}});
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
            const double worldX = calibration->cameraPosX + object.pos.x * perpendicularX +
                                  object.pos.y * forwardX;
            const double worldY = calibration->cameraPosY + object.pos.x * perpendicularY +
                                  object.pos.y * forwardY;
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

inline bool sameDouble(double lhs, double rhs) {
    return std::bit_cast<std::uint64_t>(lhs) == std::bit_cast<std::uint64_t>(rhs);
}

inline std::string compareFrames(const std::vector<domain::ObservationFrame>& expected,
                                 const std::vector<domain::ObservationFrame>& actual) {
    if (expected.size() != actual.size()) {
        return "frame count mismatch: expected=" + std::to_string(expected.size()) +
               " actual=" + std::to_string(actual.size());
    }
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        const auto& lhs = expected[frame];
        const auto& rhs = actual[frame];
        const std::string prefix = "frame[" + std::to_string(frame) + "] ";
        if (lhs.ts != rhs.ts)
            return prefix + "timestamp mismatch";
        if (lhs.ch != rhs.ch)
            return prefix + "channel mismatch";
        if (lhs.objects.size() != rhs.objects.size())
            return prefix + "object count mismatch";
        for (std::size_t object = 0; object < lhs.objects.size(); ++object) {
            const auto& lhsObject = lhs.objects[object];
            const auto& rhsObject = rhs.objects[object];
            const std::string objectPrefix = prefix + "object[" + std::to_string(object) + "] ";
            if (lhsObject.id != rhsObject.id)
                return objectPrefix + "id mismatch";
            if (lhsObject.cls != rhsObject.cls)
                return objectPrefix + "class mismatch";
            if (!sameDouble(lhsObject.pos.x, rhsObject.pos.x) ||
                !sameDouble(lhsObject.pos.y, rhsObject.pos.y)) {
                return objectPrefix + "position mismatch";
            }
        }
    }
    return {};
}

}  // namespace transform_test
