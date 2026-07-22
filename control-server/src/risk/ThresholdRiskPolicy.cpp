#include "risk/ThresholdRiskPolicy.h"

#include <iostream>
#include <limits>

ThresholdRiskPolicy::ThresholdRiskPolicy(std::shared_ptr<IDistanceMetric> metric, double warningDistance,
                                         double dangerousDistance, int channelCount)
    : metric_(std::move(metric)),
      warningDistance_(warningDistance),
      dangerousDistance_(dangerousDistance),
      channelCount_(channelCount) {}

domain::RiskEvaluation ThresholdRiskPolicy::evaluate(domain::WorldFrame& frame) {
    domain::RiskEvaluation eval;
    eval.timestamp = frame.timestamp;

    eval.zoneLevels.resize(static_cast<size_t>(channelCount_));
    for (int ch = 0; ch < channelCount_; ++ch) {
        eval.zoneLevels[ch].zoneId = ch;
        eval.zoneLevels[ch].level = veda::RiskLevel::None;
        eval.zoneLevels[ch].minDist = -1.0;
    }

    for (auto& obj : frame.objects) {
        obj.riskLevel = veda::RiskLevel::None;
        obj.nearestObj = 0;
        obj.nearestDist = -1.0;
    }

    // 차량 기준 쿼리: 차량마다 (차량이든 사람이든) 최근접 객체 탐색
    for (auto& vehicle : frame.objects) {
        if (vehicle.cls != veda::ObjectClass::Vehicle) {
            continue;
        }

        double minDist = std::numeric_limits<double>::max();
        veda::GlobalId nearestGid = 0;

        for (const auto& other : frame.objects) {
            if (other.gid == vehicle.gid) {
                continue;
            }
            double dist = metric_->calculate(vehicle.pos, other.pos);
            if (dist < minDist) {
                minDist = dist;
                nearestGid = other.gid;
            }
        }

        if (nearestGid == 0) {
            continue;  // 비교 대상 없음 (이 차량 혼자)
        }

        vehicle.nearestObj = nearestGid;
        vehicle.nearestDist = minDist;

        if (minDist <= dangerousDistance_) {
            vehicle.riskLevel = veda::RiskLevel::Danger;
        } else if (minDist <= warningDistance_) {
            vehicle.riskLevel = veda::RiskLevel::Warning;
        } else {
            vehicle.riskLevel = veda::RiskLevel::None;
        }

        // 최근접 대상이 이 위험 상황에 관여된 당사자이므로, 그 객체의 riskLevel 도
        // 함께 갱신한다 (대시보드가 "누가 위험한지"를 개별 마커로 표시할 수 있도록).
        // 한 객체가 여러 차량의 최근접 대상일 수 있으므로 더 높은 레벨로만 덮어씀
        // (Danger 로 이미 표시된 걸 나중 순회의 Warning 이 깎아내리지 않도록).
        if (vehicle.riskLevel != veda::RiskLevel::None) {
            for (auto& other : frame.objects) {
                if (other.gid == nearestGid && vehicle.riskLevel > other.riskLevel) {
                    other.riskLevel = vehicle.riskLevel;
                }
            }
        }

        // zone 집계 — zoneId 미배정/범위 밖이면 집계 제외 (크래시 방지, 로그만 남김)
        // 주의: zone 집계는 차량의 riskLevel 만 반영한다. 사람에게 전파된 riskLevel은
        // 대시보드 표시 전용이며, HW 이벤트 통지(zoneLevels)에는 영향을 주지 않는다 —
        // HW는 이미 이 차량 자체의 판정을 통해 해당 zone 에 반영되었기 때문에 중복 집계 불필요.
        if (vehicle.zoneId < 0 || vehicle.zoneId >= channelCount_) {
            std::cerr << "[ThresholdRiskPolicy] 경고: gid=" << vehicle.gid << " zoneId 미배정 또는 범위 밖("
                      << vehicle.zoneId << "), zone 집계에서 제외\n";
            continue;
        }

        auto& zone = eval.zoneLevels[static_cast<size_t>(vehicle.zoneId)];
        if (vehicle.riskLevel > zone.level) {
            zone.level = vehicle.riskLevel;
            zone.minDist = vehicle.nearestDist;
        } else if (vehicle.riskLevel == zone.level && (zone.minDist < 0.0 || vehicle.nearestDist < zone.minDist)) {
            zone.minDist = vehicle.nearestDist;
        }
    }

    return eval;
}