#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <cstdio>

#include "network/parsing/MqttPayloadLimits.h"
#include "network/parsing/RiskMessageParser.h"

namespace {
constexpr int channelCount = 12;
int failureCount = 0;

/// 파서는 ts를 로컬 시계와 비교하므로 payload의 ts도 실행 시각에서 만든다
qint64 nowMsec() { return QDateTime::currentMSecsSinceEpoch(); }

QByteArray payloadWithTimestamp(const char* rawTemplate, qint64 timestampMsec) {
    return QString::fromUtf8(rawTemplate).arg(timestampMsec).toUtf8();
}

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
 * @brief RiskFrame의 zoneId 경계와 미배정 처리 규칙을 검사합니다.
 */
void checkZoneIdValidation() {
    const QByteArray payload = payloadWithTimestamp(R"({
        "v": 1,
        "ts": %1,
        "objects": [
            {"gid": 1, "cls": "human", "pos": {"x": -50, "y": 0}, "zoneId": 0, "riskLevel": "normal"},
            {"gid": 2, "cls": "vehicle", "pos": {"x": 50, "y": 0}, "zoneId": 7, "riskLevel": "warning"},
            {"gid": 3, "cls": "human", "pos": {"x": 0, "y": 0}, "zoneId": 8, "riskLevel": "danger"},
            {"gid": 4, "cls": "human", "pos": {"x": 0, "y": 0}, "zoneId": 12, "riskLevel": "normal"},
            {"gid": 5, "cls": "human", "pos": {"x": 0, "y": 0}, "zoneId": 1.5, "riskLevel": "normal"},
            {"gid": 6, "cls": "human", "pos": {"x": 0, "y": 0}, "riskLevel": "normal"}
        ]
    })",
                                                    nowMsec());

    RiskFrameData frame;
    check(parse(payload, frame), "valid v1 frame must parse");
    check(frame.objects.size() == 6, "all supported objects must be retained");
    check(frame.objects.value(0).zoneId == 0, "zoneId 0 must be accepted");
    check(frame.objects.value(1).zoneId == 7, "zoneId 7 must be accepted");
    check(frame.objects.value(2).zoneId == 8, "zoneId 8 must be accepted for a third area");
    check(frame.objects.value(3).zoneId == -1, "out-of-range zoneId must become unassigned");
    check(frame.objects.value(4).zoneId == -1, "non-integer zoneId must become unassigned");
    check(frame.objects.value(5).zoneId == -1, "missing zoneId must become unassigned");
}

void checkOtherVersionRejected() {
    RiskFrameData frame;
    check(!parse(R"({"v":2,"ts":1,"objects":[]})", frame), "other RiskFrame version must be rejected");
}

/**
 * @brief 로컬 시계 창을 벗어난 ts가 거부되는지 검사합니다.
 *
 * @details 미래 ts는 BlurProcessor가 뒤이어 오는 정상 metadata를 전부 과거로 보고
 *          버리게 만들어 블러를 조용히 끈다. 같은 검사를 두 파서가 공유한다.
 */
void checkTimestampWindow() {
    constexpr const char* frameTemplate = R"({"v":1,"ts":%1,"objects":[]})";
    RiskFrameData frame;

    check(parse(payloadWithTimestamp(frameTemplate, nowMsec()), frame), "a current ts must be accepted");
    check(parse(payloadWithTimestamp(frameTemplate, nowMsec() - maximumPastTimestampMsec / 2), frame),
          "a recently delayed ts must still be accepted");
    check(!parse(payloadWithTimestamp(frameTemplate, nowMsec() + maximumFutureTimestampMsec + 60000), frame),
          "a far future ts must be rejected before it can silence the blur pipeline");
    check(!parse(payloadWithTimestamp(frameTemplate, nowMsec() - maximumPastTimestampMsec - 60000), frame),
          "a far past ts must be rejected");
    check(!parse(payloadWithTimestamp(frameTemplate, 0), frame), "a zero ts must stay rejected");
}

/** @brief 객체 수 상한을 넘는 프레임이 잘리지 않고 통째로 거부되는지 검사합니다. */
void checkObjectCountCeiling() {
    const auto framePayload = [](qsizetype objectCount) {
        QString objects;
        for (qsizetype index = 0; index < objectCount; ++index) {
            if (index > 0) {
                objects += QStringLiteral(",");
            }
            objects +=
                QStringLiteral(R"({"gid":%1,"cls":"human","pos":{"x":0,"y":0},"riskLevel":"normal"})").arg(index + 1);
        }
        return QStringLiteral(R"({"v":1,"ts":%1,"objects":[%2]})").arg(nowMsec()).arg(objects).toUtf8();
    };

    RiskFrameData frame;
    check(parse(framePayload(maximumRiskObjectsPerFrame), frame), "a frame at the object ceiling must be accepted");
    check(frame.objects.size() == maximumRiskObjectsPerFrame, "every object at the ceiling must be kept");
    check(!parse(framePayload(maximumRiskObjectsPerFrame + 1), frame),
          "a frame past the object ceiling must be rejected instead of truncated");
}
}  // namespace

int main() {
    checkZoneIdValidation();
    checkOtherVersionRejected();
    checkTimestampWindow();
    checkObjectCountCeiling();

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("RiskMessageParser checks passed\n");
    return 0;
}
