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
        if (c.channelId < 0) {
            continue;
        }
        maxChannel = std::max(maxChannel, static_cast<std::size_t>(c.channelId));
        any = true;
    }
    if (any) {
        byChannel_.resize(maxChannel + 1);
    }
    for (const auto& c : calibrations) {
        if (c.channelId < 0) {
            continue;
        }
        auto& slot = byChannel_[static_cast<std::size_t>(c.channelId)];
        if (!slot.has_value()) {  // 중복 항목은 첫 번째를 사용 (기존 선형 탐색의 first-match 동작 보존)
            CompiledCalibration compiled;
            compiled.cameraPosX = c.cameraPosX;
            compiled.cameraPosY = c.cameraPosY;
            compiled.facingAngleDeg = c.facingAngleDeg;
            compiled.lateralSign = c.lateralSign;
            compiled.rotationPrecomputed = std::isfinite(c.facingAngleDeg);
            if (compiled.rotationPrecomputed) {
                const double thetaRad = (90.0 - c.facingAngleDeg) * kPi / 180.0;
                compiled.forwardX = std::cos(thetaRad);
                compiled.forwardY = std::sin(thetaRad);
                compiled.perpendicularX = (c.lateralSign > 0) ? compiled.forwardY : -compiled.forwardY;
                compiled.perpendicularY = (c.lateralSign > 0) ? -compiled.forwardX : compiled.forwardX;
            }
            slot = compiled;
        }
    }
}

void AffineLocalToWorldTransform::transform(const std::vector<veda::TopViewFrame>& in,
                                            std::vector<domain::ObservationFrame>& out) {
    // 프레임별 objects 벡터까지 재사용한다. clear()+push_back은 바깥 벡터의 capacity만 남기고
    // 각 ObservationFrame을 파괴해 내부 capacity를 매 호출 잃어버린다.
    out.resize(in.size());

    const auto recordInvalidWorldCoordinate = [this](veda::ChannelId channel, double worldX, double worldY) {
        ++outOfBoundsCount_;
        if (outOfBoundsCount_ == 1 || outOfBoundsCount_ % 100 == 0) {
            logError(kIface,
                     "채널 " + std::to_string(channel) + " 월드 좌표(" + std::to_string(worldX) + ", " +
                         std::to_string(worldY) + ")가 비유한 값임 - 폐기 (누적 " +
                         std::to_string(outOfBoundsCount_) + "건)");
        }
    };

    for (std::size_t frameIndex = 0; frameIndex < in.size(); ++frameIndex) {
        const auto& frame = in[frameIndex];
        const CompiledCalibration* cal = nullptr;
        if (frame.ch >= 0 && static_cast<std::size_t>(frame.ch) < byChannel_.size()) {
            const auto& slot = byChannel_[static_cast<std::size_t>(frame.ch)];
            if (slot.has_value()) {
                cal = &slot.value();
            }
        }

        auto& observed = out[frameIndex];
        observed.ts = frame.ts;
        observed.ch = frame.ch;
        observed.objects.clear();

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
                for (const auto& obj : frame.objects) {
                    observed.objects.push_back(
                        domain::WorldObservation{obj.id, obj.cls, domain::WorldPoint{obj.pos.x, obj.pos.y}});
                }
            }
            continue;
        }

        double forwardX = cal->forwardX;
        double forwardY = cal->forwardY;
        double perpendicularX = cal->perpendicularX;
        double perpendicularY = cal->perpendicularY;
        if (!cal->rotationPrecomputed) {
            const double thetaRad = (90.0 - cal->facingAngleDeg) * kPi / 180.0;
            forwardX = std::cos(thetaRad);
            forwardY = std::sin(thetaRad);
            perpendicularX = (cal->lateralSign > 0) ? forwardY : -forwardY;
            perpendicularY = (cal->lateralSign > 0) ? -forwardX : forwardX;
        }

        if (!bounds_.enabled) {
            observed.objects.resize(frame.objects.size());
            std::size_t outputIndex = 0;
            for (std::size_t objectIndex = 0; objectIndex < frame.objects.size(); ++objectIndex) {
                const auto& object = frame.objects[objectIndex];
                const double worldX =
                    cal->cameraPosX + object.pos.x * perpendicularX + object.pos.y * forwardX;
                const double worldY =
                    cal->cameraPosY + object.pos.x * perpendicularY + object.pos.y * forwardY;
                if (!std::isfinite(worldX) || !std::isfinite(worldY)) {
                    recordInvalidWorldCoordinate(frame.ch, worldX, worldY);
                    continue;
                }

                auto& transformed = observed.objects[outputIndex++];
                transformed.id = object.id;
                transformed.cls = object.cls;
                transformed.pos.x = worldX;
                transformed.pos.y = worldY;
            }
            observed.objects.resize(outputIndex);
            continue;
        }

        observed.objects.reserve(frame.objects.size());
        for (const auto& obj : frame.objects) {
            const double worldX = cal->cameraPosX + obj.pos.x * perpendicularX + obj.pos.y * forwardX;
            const double worldY = cal->cameraPosY + obj.pos.x * perpendicularY + obj.pos.y * forwardY;

            if (!std::isfinite(worldX) || !std::isfinite(worldY)) {
                recordInvalidWorldCoordinate(frame.ch, worldX, worldY);
                continue;
            }

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
    }
}
