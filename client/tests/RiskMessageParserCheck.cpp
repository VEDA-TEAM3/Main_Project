#include <QByteArray>
#include <cstdio>

#include "network/parsing/RiskMessageParser.h"

namespace {
constexpr int channelCount = 12;
int failureCount = 0;

void check(bool condition, const char* description) {
    if (condition) {
        return;
    }

    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failureCount;
}

bool parse(const QByteArray& payload, RiskFrameData& frame) {
    QString error;
    return RiskMessageParser::parse(payload, QStringLiteral("veda/risk"), frame, error, channelCount);
}

/**
 * @brief RiskFrame v2의 zoneId 경계와 미배정 처리 규칙을 검사합니다.
 */
void checkZoneIdValidation() {
    const QByteArray payload = R"({
        "v": 2,
        "ts": 1786300000000,
        "objects": [
            {"gid": 1, "cls": "human", "pos": {"x": -50, "y": 0}, "zoneId": 0, "riskLevel": "normal"},
            {"gid": 2, "cls": "vehicle", "pos": {"x": 50, "y": 0}, "zoneId": 7, "riskLevel": "warning"},
            {"gid": 3, "cls": "human", "pos": {"x": 0, "y": 0}, "zoneId": 8, "riskLevel": "danger"},
            {"gid": 4, "cls": "human", "pos": {"x": 0, "y": 0}, "zoneId": 12, "riskLevel": "normal"},
            {"gid": 5, "cls": "human", "pos": {"x": 0, "y": 0}, "zoneId": 1.5, "riskLevel": "normal"},
            {"gid": 6, "cls": "human", "pos": {"x": 0, "y": 0}, "riskLevel": "normal"}
        ]
    })";

    RiskFrameData frame;
    check(parse(payload, frame), "valid v2 frame must parse");
    check(frame.objects.size() == 6, "all supported objects must be retained");
    check(frame.objects.value(0).zoneId == 0, "zoneId 0 must be accepted");
    check(frame.objects.value(1).zoneId == 7, "zoneId 7 must be accepted");
    check(frame.objects.value(2).zoneId == 8, "zoneId 8 must be accepted for a third area");
    check(frame.objects.value(3).zoneId == -1, "out-of-range zoneId must become unassigned");
    check(frame.objects.value(4).zoneId == -1, "non-integer zoneId must become unassigned");
    check(frame.objects.value(5).zoneId == -1, "missing zoneId must become unassigned");
}

void checkLegacyVersionRejected() {
    RiskFrameData frame;
    check(!parse(R"({"v":1,"ts":1,"objects":[]})", frame), "legacy RiskFrame version must be rejected");
}
}  // namespace

int main() {
    checkZoneIdValidation();
    checkLegacyVersionRejected();

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("RiskMessageParser checks passed\n");
    return 0;
}
