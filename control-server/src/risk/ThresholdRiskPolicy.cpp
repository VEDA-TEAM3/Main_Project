#include "risk/ThresholdRiskPolicy.h"

#include <limits>
#include <string>

#include "Logger.h"

namespace {
constexpr const char* kIface = "RiskPolicy";
}  // namespace

ThresholdRiskPolicy::ThresholdRiskPolicy(std::shared_ptr<IDistanceMetric> metric, const RiskConfig& risk,
                                         int channelCount)
    : metric_(std::move(metric)),
      warningDistance_(risk.warningDistance),
      dangerousDistance_(risk.dangerousDistance),
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

        if (vehicle.riskLevel != veda::RiskLevel::None) {
            // 차량이 가까이 있는 동안 윈도우마다(초당 10회) 같은 판정이 반복되므로 rate-limit
            ++riskLogCount_;
            if (riskLogCount_ == 1 || riskLogCount_ % 50 == 0) {
                logSuccess(kIface,
                           "gid=" + std::to_string(vehicle.gid) + " " + std::string(veda::toString(vehicle.riskLevel)) +
                               " 판정 (최근접 gid=" + std::to_string(nearestGid) + ", 거리=" + std::to_string(minDist) +
                               "m, 누적 " + std::to_string(riskLogCount_) + "건)");
            }
        }

        // [Rule 3] 상호 위험 부여: 판정된 위험 레벨을 차량뿐 아니라 그 위험에 얽힌
        // 최근접 객체(사람이든 차량이든)에도 부여한다 (대시보드가 "누가 위험한지"를 개별
        // 마커로 표시). 한 객체가 여러 차량의 최근접 대상일 수 있으므로 더 높은 레벨로만
        // 덮어쓴다 (Danger 로 이미 표시된 걸 나중 순회의 Warning 이 깎아내리지 않도록).
        if (vehicle.riskLevel != veda::RiskLevel::None) {
            for (auto& other : frame.objects) {
                if (other.gid == nearestGid && vehicle.riskLevel > other.riskLevel) {
                    other.riskLevel = vehicle.riskLevel;
                }
            }
        }

        // [Rule 5] 채널(zone) 위험도 = 그 채널 안 '차량'들의 riskLevel max.
        // 사람에게 전파된 riskLevel(Rule 3)은 여기 집계하지 않는다 — HW/UI 로 나가는
        // 채널 위험도는 오직 차량 판정으로만 결정된다 (그 위험은 이미 차량 자신을 통해
        // 해당 zone 에 반영됨).
        // zoneId 미배정/범위 밖이면 집계 제외 (크래시 방지, 로그만 남김).
        if (vehicle.zoneId < 0 || vehicle.zoneId >= channelCount_) {
            logError(kIface, "gid=" + std::to_string(vehicle.gid) + " zoneId 미배정 또는 범위 밖(" +
                                 std::to_string(vehicle.zoneId) + "), zone 집계에서 제외");
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

    // [Rule 4] UI == HW 단일 진실 공급원.
    // 프레임 전체 위험도(UI RiskFrame.level 이 읽음)를 여기서 '채널별 위험도(zoneLevels,
    // HW/UART 로 나가는 값)의 max' 로 확정해 frame 에 실어 보낸다. sink(MqttTransport)는
    // 이 값을 그대로 읽기만 하고 재계산하지 않으므로, UI 전체 위험도와 HW 채널 위험도가
    // 반드시 같은 소스에서 파생된다.
    veda::RiskLevel overall = veda::RiskLevel::None;
    for (const auto& zone : eval.zoneLevels) {
        if (zone.level > overall)
            overall = zone.level;
    }
    frame.level = overall;

    return eval;
}