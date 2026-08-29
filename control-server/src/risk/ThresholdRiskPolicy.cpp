#include "risk/ThresholdRiskPolicy.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include "Logger.h"

namespace {
constexpr const char* kIface = "RiskPolicy";
constexpr int kMaxChannelCount = 256;

bool isFinitePoint(const domain::WorldPoint& p) { return std::isfinite(p.x) && std::isfinite(p.y); }
}  // namespace

ThresholdRiskPolicy::ThresholdRiskPolicy(std::shared_ptr<IDistanceMetric> metric, const RiskConfig& risk,
                                         int channelCount)
    : metric_(std::move(metric)),
      warningDistance_(risk.warningDistance),
      dangerousDistance_(risk.dangerousDistance),
      channelCount_(channelCount) {
    // 조립 시점 fail-fast. AppConfig 가 일부를 보정하지만 DI 로 직접 넣는 경로는
    // 막지 못한다.
    if (!metric_) {
        throw std::invalid_argument("risk policy requires a non-null distance metric");
    }
    if (channelCount_ < 1 || channelCount_ > kMaxChannelCount) {
        throw std::invalid_argument("risk policy channelCount out of range [1, 256]");
    }
    // 비유한 임계값은 '조용한 무경보'가 된다 -- NaN 은 <= 비교에서 항상 false 라
    // Danger/Warning 어느 쪽도 성립하지 않는다. 음수도 마찬가지로 영원히 성립하지
    // 않는다.
    if (!std::isfinite(warningDistance_) || !std::isfinite(dangerousDistance_) || warningDistance_ < 0.0 ||
        dangerousDistance_ < 0.0) {
        throw std::invalid_argument("risk distances must be finite and non-negative");
    }
    // 역전되면 Warning 이 영영 도달 불가능해진다 (dangerous 이하가 전부 Danger 로
    // 먹힘). 과다 경보라 안전 방향이긴 하나, 운영자가 의도한 2단계 판정이 조용히
    // 1단계가 된다.
    if (dangerousDistance_ > warningDistance_) {
        throw std::invalid_argument("dangerousDistance must be <= warningDistance");
    }
}

void ThresholdRiskPolicy::evaluate(domain::WorldFrame& frame, domain::RiskEvaluation& out) {
    out.timestamp = frame.timestamp;

    // resize 는 정상 운용에서 매 프레임 같은 값이라 사실상 no-op 이다 (호출자가
    // 버퍼를 재사용).
    out.zoneLevels.resize(static_cast<std::size_t>(channelCount_));
    for (int ch = 0; ch < channelCount_; ++ch) {
        auto& zone = out.zoneLevels[static_cast<std::size_t>(ch)];
        zone.zoneId = ch;
        zone.level = veda::RiskLevel::None;
        zone.minDist = -1.0;
    }

    // ---- 1단계: O(N) 초기화 + 유한성 검사 + 좌표 SoA 구축 ----
    // 기존에도 있던 초기화 루프에 검사와 SoA 구축을 얹었다. 순회 횟수는 그대로 N
    // 이다.
    //
    // [ 유한성 검사를 여기서 하는 이유 ]
    // NaN 좌표는 거리도 NaN 으로 만들고, NaN 은 '<' 와 '<=' 모두에서 false 다.
    // 그러면
    //   - dist < minDist        가 false  -> 최근접으로 뽑히지 않음
    //   - minDist <= dangerous  가 false  -> Danger 판정 안 됨
    // 즉 그 객체와 얽힌 차량이 판정에서 통째로 빠지고 경보가 조용히 사라진다.
    // 이중 루프 안에서 검사하면 비용이 O(N^2) 이 되므로 여기서 한 번만 걸러 낸다.
    candPos_.clear();
    candIdx_.clear();
    candPos_.reserve(frame.objects.size());
    candIdx_.reserve(frame.objects.size());

    std::size_t droppedThisFrame = 0;
    for (std::size_t i = 0; i < frame.objects.size(); ++i) {
        auto& obj = frame.objects[i];
        obj.riskLevel = veda::RiskLevel::None;
        obj.nearestObj = 0;
        obj.nearestDist = -1.0;

        if (obj.sourceChannels.empty()) {
            continue;  // coast 객체는 표시·추적용이며 현재 위험 판정에는 쓰지 않는다
        }
        if (!isFinitePoint(obj.pos)) {
            ++droppedThisFrame;
            continue;  // 위치를 모르는 객체는 판정에 참여시키지 않는다
        }
        candPos_.push_back(obj.pos);
        candIdx_.push_back(static_cast<std::uint32_t>(i));
    }

    if (droppedThisFrame > 0) {
        nonFiniteCount_ += droppedThisFrame;
        if (nonFiniteCount_ == 1 || nonFiniteCount_ % 100 == 0) {
            logError(kIface, "비유한 좌표 객체 " + std::to_string(droppedThisFrame) + "개를 판정에서 제외 (누적 " +
                                 std::to_string(nonFiniteCount_) + "건)");
        }
    }

    // ---- 2단계: 차량 기준 최근접 탐색 (V x N) ----
    for (std::size_t k = 0; k < candIdx_.size(); ++k) {
        const std::uint32_t vehicleIdx = candIdx_[k];
        auto& vehicle = frame.objects[vehicleIdx];
        if (vehicle.cls != veda::ObjectClass::Vehicle) {
            continue;
        }

        const domain::WorldPoint vehiclePos = candPos_[k];
        double minDist = std::numeric_limits<double>::max();
        std::uint32_t nearestIdx = kNoIndex;

        // 좌표만 담긴 연속 배열을 훑는다. metric_ 호출은 유지 -- 거리 정의를 DI 로
        // 바꿀 수 있다는 것이 이 계층의 계약이므로, 여기서 유클리드를 인라인하면 그
        // seam 이 죽는다.
        for (std::size_t m = 0; m < candPos_.size(); ++m) {
            if (m == k) {
                continue;  // 인덱스 비교 -- gid 비교보다 싸고, gid 중복에도 안전
            }
            const double dist = metric_->calculate(vehiclePos, candPos_[m]);
            // 유한성이 1단계에서 보장되므로 이 비교는 NaN 을 만나지 않는다.
            // 컴파일러가 cmov 쌍으로 낮추기 좋은 형태 (분기 예측 실패 비용 없음).
            const bool better = dist < minDist;
            minDist = better ? dist : minDist;
            nearestIdx = better ? candIdx_[m] : nearestIdx;
        }

        if (nearestIdx == kNoIndex) {
            continue;  // 비교 대상 없음 (이 차량 혼자)
        }

        auto& nearest = frame.objects[nearestIdx];
        vehicle.nearestObj = nearest.gid;
        vehicle.nearestDist = minDist;

        // 3단 분류. 호출 횟수가 V 라 분기 비용이 무의미하므로 가독성을 택한다.
        if (minDist <= dangerousDistance_) {
            vehicle.riskLevel = veda::RiskLevel::Danger;
        } else if (minDist <= warningDistance_) {
            vehicle.riskLevel = veda::RiskLevel::Warning;
        } else {
            vehicle.riskLevel = veda::RiskLevel::None;
        }

        if (vehicle.riskLevel == veda::RiskLevel::None) {
            continue;
        }

        // 차량이 가까이 있는 동안 윈도우마다(초당 10회) 같은 판정이 반복되므로
        // rate-limit. 문자열 조립은 rate-limit 을 통과한 뒤에만 한다 (핫패스 할당
        // 방지).
        ++riskLogCount_;
        if ((riskLogCount_ == 1 || riskLogCount_ % 50 == 0) && isLogEnabled(LogLevel::Info)) {
            logSuccess(kIface,
                       "gid=" + std::to_string(vehicle.gid) + " " + std::string(veda::toString(vehicle.riskLevel)) +
                           " 판정 (최근접 gid=" + std::to_string(nearest.gid) + ", 거리=" + std::to_string(minDist) +
                           "m, 누적 " + std::to_string(riskLogCount_) + "건)");
        }

        // [Rule 3] 상호 위험 부여: 판정된 위험 레벨을 차량뿐 아니라 그 위험에 얽힌
        // 최근접 객체(사람이든 차량이든)에도 부여한다 (대시보드가 "누가 위험한지"를
        // 개별 마커로 표시). 한 객체가 여러 차량의 최근접 대상일 수 있으므로 더
        // 높은 레벨로만 덮어쓴다 (Danger 로 이미 표시된 걸 나중 순회의 Warning 이
        // 깎아내리지 않도록).
        //
        // 인덱스를 그대로 쓴다 -- 예전에는 gid 로 다시 전체 순회해서 찾았고, 그
        // 재탐색이 위험 차량마다 O(N) 이었다.
        if (vehicle.riskLevel > nearest.riskLevel) {
            nearest.riskLevel = vehicle.riskLevel;
        }

        // [Rule 5] 채널(zone) 위험도 = 그 채널 안 '차량'들의 riskLevel max.
        // 사람에게 전파된 riskLevel(Rule 3)은 여기 집계하지 않는다 — HW/UI 로
        // 나가는 채널 위험도는 오직 차량 판정으로만 결정된다 (그 위험은 이미 차량
        // 자신을 통해 해당 zone 에 반영됨). zoneId 미배정/범위 밖이면 집계 제외
        // (크래시 방지, 로그만 남김).
        if (vehicle.zoneId < 0 || vehicle.zoneId >= channelCount_) {
            logError(kIface, "gid=" + std::to_string(vehicle.gid) + " zoneId 미배정 또는 범위 밖(" +
                                 std::to_string(vehicle.zoneId) + "), zone 집계에서 제외");
            continue;
        }

        auto& zone = out.zoneLevels[static_cast<std::size_t>(vehicle.zoneId)];
        if (vehicle.riskLevel > zone.level) {
            zone.level = vehicle.riskLevel;
            zone.minDist = vehicle.nearestDist;
        } else if (vehicle.riskLevel == zone.level && (zone.minDist < 0.0 || vehicle.nearestDist < zone.minDist)) {
            zone.minDist = vehicle.nearestDist;
        }
    }

    // [Rule 4] UI == HW 단일 진실 공급원.
    // 프레임 전체 위험도(UI RiskFrame.level 이 읽음)를 여기서 '채널별
    // 위험도(zoneLevels, HW/UART 로 나가는 값)의 max' 로 확정해 frame 에 실어
    // 보낸다. sink(MqttTransport)는 이 값을 그대로 읽기만 하고 재계산하지
    // 않으므로, UI 전체 위험도와 HW 채널 위험도가 반드시 같은 소스에서 파생된다.
    veda::RiskLevel overall = veda::RiskLevel::None;
    for (const auto& zone : out.zoneLevels) {
        if (zone.level > overall) {
            overall = zone.level;
        }
    }
    frame.level = overall;
}
