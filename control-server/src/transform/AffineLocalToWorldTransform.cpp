#include "transform/AffineLocalToWorldTransform.h"

#include <cmath>
#include <iostream>

namespace {
constexpr double kPi = 3.14159265358979323846;
}

AffineLocalToWorldTransform::AffineLocalToWorldTransform(std::vector<CameraCalibration> calibrations)
    : calibrations_(std::move(calibrations)) {}

const CameraCalibration* AffineLocalToWorldTransform::find(veda::ChannelId ch) const {
    for (const auto& c : calibrations_) {
        if (c.channelId == ch) return &c;
    }
    return nullptr;
}

void AffineLocalToWorldTransform::transform(std::vector<veda::TopViewFrame>& frames) {
    for (auto& frame : frames) {
        const CameraCalibration* cal = find(frame.ch);
        if (!cal) {
            std::cerr << "[AffineLocalToWorldTransform] 경고: 채널 " << frame.ch
                      << " 캘리브레이션 없음 — 좌표 변환 생략, 로컬 좌표 그대로 통과\n";
            continue;
        }

        const double thetaRad = (90.0 - cal->facingAngleDeg) * kPi / 180.0;
        const double fx = std::cos(thetaRad);
        const double fy = std::sin(thetaRad);

        const double perpX = (cal->lateralSign > 0) ? fy : -fy;
        const double perpY = (cal->lateralSign > 0) ? -fx : fx;

        for (auto& obj : frame.objects) {
            const double localX = obj.pos.x;
            const double localY = obj.pos.y;
            obj.pos.x = cal->cameraPosX + localX * perpX + localY * fx;
            obj.pos.y = cal->cameraPosY + localX * perpY + localY * fy;
        }
    }
}