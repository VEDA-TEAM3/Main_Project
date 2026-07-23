#include "transform/AffineLocalToWorldTransform.h"

#include <cmath>
#include <string>

#include "Logger.h"

namespace {
constexpr const char* kIface = "Transform";
constexpr double kPi = 3.14159265358979323846;
}  // namespace

AffineLocalToWorldTransform::AffineLocalToWorldTransform(std::vector<CameraCalibration> calibrations,
                                                         bool dropUncalibrated)
    : calibrations_(std::move(calibrations)), dropUncalibrated_(dropUncalibrated) {}

const CameraCalibration* AffineLocalToWorldTransform::find(veda::ChannelId ch) const {
    for (const auto& c : calibrations_) {
        if (c.channelId == ch)
            return &c;
    }
    return nullptr;
}

void AffineLocalToWorldTransform::transform(std::vector<veda::TopViewFrame>& frames) {
    for (auto& frame : frames) {
        const CameraCalibration* cal = find(frame.ch);
        if (!cal) {
            // 캘리브레이션이 없으면 로컬 좌표를 도면 좌표로 옮길 방법이 없음
            // 그대로 통과시키면 '카메라 전방 기준' 좌표가 '도면 기준'인 척 위험 판정과
            // zone 배정에 들어가므로, 조용히 틀린 값을 쓰느니 버리는 게 안전함
            // (compute-server 의 riskEdgePolicy 와 같은 논리)
            ++uncalibratedCount_;
            if (uncalibratedCount_ == 1 || uncalibratedCount_ % 100 == 0) {
                logError(kIface, "채널 " + std::to_string(frame.ch) + " 캘리브레이션 없음 — " +
                                     (dropUncalibrated_ ? "객체 폐기" : "로컬 좌표 그대로 통과(위험)") + " (누적 " +
                                     std::to_string(uncalibratedCount_) + "건)");
            }
            if (dropUncalibrated_)
                frame.objects.clear();
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