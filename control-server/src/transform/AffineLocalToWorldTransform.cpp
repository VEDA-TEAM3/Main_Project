#include "transform/AffineLocalToWorldTransform.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

#include "Logger.h"

namespace {
constexpr const char* kIface = "Transform";
constexpr double kPi = 3.14159265358979323846;
}  // namespace

AffineLocalToWorldTransform::AffineLocalToWorldTransform(std::vector<CameraCalibration> calibrations,
                                                         bool dropUncalibrated, WorldBounds bounds)
    : bounds_(bounds), dropUncalibrated_(dropUncalibrated) {
    // channelId 로 바로 색인할 수 있게 펼친다 (프레임마다의 선형 탐색 제거)
    std::size_t maxChannel = 0;
    bool any = false;
    for (const auto& c : calibrations) {
        if (c.channelId < 0)
            continue;
        maxChannel = std::max(maxChannel, static_cast<std::size_t>(c.channelId));
        any = true;
    }
    if (any)
        byChannel_.resize(maxChannel + 1);

    for (const auto& c : calibrations) {
        if (c.channelId < 0)
            continue;
        auto& slot = byChannel_[static_cast<std::size_t>(c.channelId)];
        if (!slot.has_value())  // 중복 항목은 첫 번째를 사용 (기존 선형 탐색의 first-match 동작 보존)
            slot = c;
    }
}

void AffineLocalToWorldTransform::transform(const std::vector<veda::TopViewFrame>& in,
                                            std::vector<domain::ObservationFrame>& out) {
    out.clear();  // capacity 는 유지 -> 윈도우마다 재할당하지 않음
    out.reserve(in.size());

    for (const auto& frame : in) {
        const CameraCalibration* cal = nullptr;
        if (frame.ch >= 0 && static_cast<std::size_t>(frame.ch) < byChannel_.size()) {
            const auto& slot = byChannel_[static_cast<std::size_t>(frame.ch)];
            if (slot.has_value())
                cal = &slot.value();
        }

        domain::ObservationFrame observed;
        observed.ts = frame.ts;
        observed.ch = frame.ch;

        if (cal == nullptr) {
            // 캘리브레이션이 없으면 로컬 좌표를 도면 좌표로 옮길 방법이 없음.
            // 그대로 통과시키면 '카메라 전방 기준' 좌표가 '도면 기준'인 척 위험 판정과 zone 배정에
            // 들어가므로, 조용히 틀린 값을 쓰느니 버리는 게 안전함 (compute-server 의 riskEdgePolicy 와 동일 논리)
            ++uncalibratedCount_;
            if (uncalibratedCount_ == 1 || uncalibratedCount_ % 100 == 0) {
                logError(kIface, "채널 " + std::to_string(frame.ch) + " 캘리브레이션 없음 — " +
                                     (dropUncalibrated_ ? "객체 폐기" : "로컬 좌표 그대로 통과(위험)") + " (누적 " +
                                     std::to_string(uncalibratedCount_) + "건)");
            }
            if (!dropUncalibrated_) {
                observed.objects.reserve(frame.objects.size());
                for (const auto& obj : frame.objects)
                    observed.objects.push_back(
                        domain::WorldObservation{obj.id, obj.cls, domain::WorldPoint{obj.pos.x, obj.pos.y}});
            }
            out.push_back(std::move(observed));
            continue;
        }

        // 나침반 방위(북=0, 시계방향) -> atan2 규약으로 변환한 뒤 전방/측방 단위벡터를 구함
        const double thetaRad = (90.0 - cal->facingAngleDeg) * kPi / 180.0;
        const double fx = std::cos(thetaRad);
        const double fy = std::sin(thetaRad);
        const double perpX = (cal->lateralSign > 0) ? fy : -fy;
        const double perpY = (cal->lateralSign > 0) ? -fx : fx;

        observed.objects.reserve(frame.objects.size());
        for (const auto& obj : frame.objects) {
            const double worldX = cal->cameraPosX + obj.pos.x * perpX + obj.pos.y * fx;
            const double worldY = cal->cameraPosY + obj.pos.x * perpY + obj.pos.y * fy;

            if (bounds_.enabled && (worldX < bounds_.minX || worldX > bounds_.maxX || worldY < bounds_.minY ||
                                    worldY > bounds_.maxY)) {
                // 도면 밖으로 사상됐다는 건 캘리브레이션 오류(cameraPos 오타 등)를 뜻함.
                // 그냥 두면 zone 배정/위험 판정이 조용히 틀어지므로 폐기
                // (compute-server 의 localBounds 와 동일한 정책)
                ++outOfBoundsCount_;
                if (outOfBoundsCount_ == 1 || outOfBoundsCount_ % 100 == 0) {
                    logError(kIface, "채널 " + std::to_string(frame.ch) + " 월드 좌표(" + std::to_string(worldX) +
                                         ", " + std::to_string(worldY) + ")가 도면 범위를 벗어남 - 폐기 (누적 " +
                                         std::to_string(outOfBoundsCount_) + "건)");
                }
                continue;
            }

            observed.objects.push_back(domain::WorldObservation{obj.id, obj.cls, domain::WorldPoint{worldX, worldY}});
        }

        out.push_back(std::move(observed));
    }
}
