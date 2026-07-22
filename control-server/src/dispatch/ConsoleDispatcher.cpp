#include "dispatch/ConsoleDispatcher.h"

#include <string>

#include "log/Logger.h"

namespace {
constexpr const char* kIface = "Dispatcher";
}  // namespace

void ConsoleDispatcher::dispatch(const domain::RiskEvaluation& eval) {
    for (const auto& zone : eval.zoneLevels) {
        auto it = lastSentLevel_.find(zone.zoneId);
        const bool changed = (it == lastSentLevel_.end()) || (it->second != zone.level);
        if (!changed) {
            continue;
        }

        logSuccess(kIface, "UART 이벤트 통지 → 채널 " + std::to_string(zone.zoneId) + ": " +
                               std::string(veda::toString(zone.level)) + " (거리: " + std::to_string(zone.minDist) +
                               "m, 기준 시간: " + std::to_string(eval.timestamp) + ")");

        lastSentLevel_[zone.zoneId] = zone.level;
    }
}

void ConsoleDispatcher::setStatusCallback(StatusCallback callback) { statusCallback_ = std::move(callback); }