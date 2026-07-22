#include "sink/ConsoleSink.h"

#include <iostream>
#include <string>

#include "log/Logger.h"

namespace {
constexpr const char* kIface = "Sink";
}  // namespace

void ConsoleSink::send(const domain::WorldFrame& frame) {
    std::cout << "========================================================\n";
    std::cout << "[Sink] 🗺️ 최종 교차로 도면 전송 (시간: " << frame.timestamp << ")\n";
    std::cout << "현재 융합된 객체 수: " << frame.objects.size() << "개\n";
    std::cout << "--------------------------------------------------------\n";

    for (const auto& obj : frame.objects) {
        std::cout << " * [G-ID: " << obj.gid << "] ";

        if (obj.cls == veda::ObjectClass::Vehicle)
            std::cout << "🚗 차량 ";
        else if (obj.cls == veda::ObjectClass::Human)
            std::cout << "🚶 보행자 ";
        else
            std::cout << "❓ 알수없음 ";

        std::cout << "| 위치: (" << obj.pos.x << ", " << obj.pos.y << ") ";

        if (obj.riskLevel == veda::RiskLevel::Danger) {
            std::cout << "| 🔴 DANGER ";
        } else if (obj.riskLevel == veda::RiskLevel::Warning) {
            std::cout << "| 🟡 WARNING ";
        } else {
            std::cout << "| 🟢 SAFE ";
        }

        if (obj.nearestObj != 0) {
            std::cout << "| 타겟 ID " << obj.nearestObj << " (거리: " << obj.nearestDist << "m)\n";
        } else {
            std::cout << "| 주변 객체 없음\n";
        }
    }
    std::cout << "========================================================\n\n";

    int dangerCount = 0;
    int warningCount = 0;
    for (const auto& obj : frame.objects) {
        if (obj.riskLevel == veda::RiskLevel::Danger)
            ++dangerCount;
        else if (obj.riskLevel == veda::RiskLevel::Warning)
            ++warningCount;
    }
    logSuccess(kIface, std::to_string(frame.objects.size()) + "개 객체 전송 (Danger " + std::to_string(dangerCount) +
                           "개, Warning " + std::to_string(warningCount) + "개)");
}