#include "dispatch/ConsoleDispatcher.h"

#include <iostream>

void ConsoleDispatcher::dispatch(const domain::RiskEvaluation& eval) {
    for (const auto& zone : eval.zoneLevels) {
        auto it = lastSentLevel_.find(zone.zoneId);
        const bool changed = (it == lastSentLevel_.end()) || (it->second != zone.level);
        if (!changed) {
            continue;
        }

        std::cout << "[Dispatcher] UART 이벤트 통지 → 채널 " << zone.zoneId << ": ";
        switch (zone.level) {
            case veda::RiskLevel::Danger:
                std::cout << "DANGER";
                break;
            case veda::RiskLevel::Warning:
                std::cout << "WARNING";
                break;
            case veda::RiskLevel::None:
            default:
                std::cout << "NONE";
                break;
        }
        std::cout << " (거리: " << zone.minDist << "m, 기준 시간: " << eval.timestamp << ")\n";

        lastSentLevel_[zone.zoneId] = zone.level;
    }
}

void ConsoleDispatcher::setStatusCallback(StatusCallback callback) { statusCallback_ = std::move(callback); }