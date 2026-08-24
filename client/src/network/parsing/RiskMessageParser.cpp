#include "network/parsing/RiskMessageParser.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <utility>

#include "network/parsing/MqttJsonValue.h"
#include "network/parsing/MqttPayloadLimits.h"

namespace {
constexpr int riskProtocolVersion = 1;
constexpr int minimumZoneId = 0;

bool parseRiskLevel(const QJsonValue& value, DigitalTwinRiskLevel& riskLevel) {
    if (!value.isString()) {
        return false;
    }

    const QString text = value.toString().trimmed().toLower();
    if (text == QStringLiteral("none") || text == QStringLiteral("normal")) {
        riskLevel = DigitalTwinRiskLevel::Normal;
        return true;
    }
    if (text == QStringLiteral("warning")) {
        riskLevel = DigitalTwinRiskLevel::Warning;
        return true;
    }
    if (text == QStringLiteral("danger")) {
        riskLevel = DigitalTwinRiskLevel::Danger;
        return true;
    }
    return false;
}

DigitalTwinRiskLevel highestRiskLevel(DigitalTwinRiskLevel first, DigitalTwinRiskLevel second) {
    return static_cast<int>(first) >= static_cast<int>(second) ? first : second;
}

int parseZoneId(const QJsonObject& object, int channelCount) {
    qint64 zoneId = -1;
    if (!readMqttInteger(object, QStringLiteral("zoneId"), zoneId) || zoneId < minimumZoneId ||
        zoneId >= channelCount) {
        return -1;
    }
    return static_cast<int>(zoneId);
}
}  // namespace

/**
 * @brief         RiskFrame MQTT payload를 화면 입력 모델로 변환합니다.
 * @param payload MQTT JSON payload
 * @param topic   수신 토픽
 * @param frame   변환된 위험 프레임
 * @param error   검증 실패 원인
 * @param channelCount 설정된 전체 채널 수
 * @return        계약 검증과 변환에 성공하면 true
 */
bool RiskMessageParser::parse(const QByteArray& payload, const QString& topic, RiskFrameData& frame, QString& error,
                              int channelCount) {
    if (channelCount <= 0) {
        error = QStringLiteral("Risk channel count must be positive");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("Invalid RiskFrame JSON on %1: %2").arg(topic, parseError.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    qint64 version = 0;
    qint64 timestamp = 0;
    if (!readMqttInteger(root, QStringLiteral("v"), version) || version != riskProtocolVersion ||
        !readMqttInteger(root, QStringLiteral("ts"), timestamp) || timestamp <= 0) {
        error = QStringLiteral("Invalid RiskFrame v or ts field on %1").arg(topic);
        return false;
    }

    // 시계 창을 벗어난 ts는 자동 경계 warmup을 앞당기고 진단 통계를 망가뜨린다
    if (!isFreshSourceTimestamp(timestamp, QDateTime::currentMSecsSinceEpoch())) {
        error = QStringLiteral("RiskFrame ts is outside the accepted clock window on %1: %2").arg(topic).arg(timestamp);
        return false;
    }

    const QJsonValue objectsValue = root.value(QStringLiteral("objects"));
    if (!objectsValue.isArray()) {
        error = QStringLiteral("Missing objects array on %1").arg(topic);
        return false;
    }

    frame = {};
    frame.sourceTimestamp = timestamp;
    if (root.contains(QStringLiteral("level")) &&
        !parseRiskLevel(root.value(QStringLiteral("level")), frame.riskLevel)) {
        error = QStringLiteral("Invalid RiskFrame level field on %1").arg(topic);
        return false;
    }

    const QJsonArray objects = objectsValue.toArray();
    // 넘치는 만큼만 잘라 쓰면 어떤 객체가 빠졌는지 알 수 없는 채로 안전 판단이 돈다
    if (objects.size() > maximumRiskObjectsPerFrame) {
        error = QStringLiteral("RiskFrame carries too many objects on %1: %2").arg(topic).arg(objects.size());
        return false;
    }

    frame.objects.reserve(objects.size());
    for (const QJsonValue& objectValue : objects) {
        if (!objectValue.isObject()) {
            error = QStringLiteral("RiskFrame objects must be JSON objects on %1").arg(topic);
            return false;
        }

        const QJsonObject sourceObject = objectValue.toObject();
        const QJsonValue classValue = sourceObject.value(QStringLiteral("cls"));
        const QJsonValue positionValue = sourceObject.value(QStringLiteral("pos"));
        const QJsonValue riskValue = sourceObject.contains(QStringLiteral("riskLevel"))
                                         ? sourceObject.value(QStringLiteral("riskLevel"))
                                         : sourceObject.value(QStringLiteral("level"));
        qint64 objectId = 0;
        DigitalTwinRiskLevel objectRiskLevel = DigitalTwinRiskLevel::Normal;
        if (!readMqttInteger(sourceObject, QStringLiteral("gid"), objectId) || objectId <= 0 ||
            !classValue.isString() || !positionValue.isObject() || !parseRiskLevel(riskValue, objectRiskLevel)) {
            error = QStringLiteral("Invalid RiskObject fields on %1").arg(topic);
            return false;
        }

        const QString objectClass = classValue.toString().trimmed().toLower();
        if (objectClass != QStringLiteral("human") && objectClass != QStringLiteral("vehicle")) {
            continue;
        }

        const QJsonObject position = positionValue.toObject();
        double x = 0.0;
        double y = 0.0;
        if (!readMqttFiniteNumber(position, QStringLiteral("x"), x) ||
            !readMqttFiniteNumber(position, QStringLiteral("y"), y)) {
            error = QStringLiteral("Invalid RiskObject world position on %1").arg(topic);
            return false;
        }

        RiskObjectData parsedObject;
        parsedObject.globalId = objectId;
        parsedObject.objectClass =
            objectClass == QStringLiteral("human") ? QStringLiteral("Human") : QStringLiteral("Vehicle");
        parsedObject.worldPosition = QPointF(x, y);
        parsedObject.riskLevel = objectRiskLevel;
        parsedObject.zoneId = parseZoneId(sourceObject, channelCount);

        qint64 nearestId = 0;
        if (readMqttInteger(sourceObject, QStringLiteral("nearest"), nearestId) && nearestId >= 0) {
            parsedObject.nearestId = nearestId;
        }
        // 음수는 RiskObjectData가 "거리 없음"을 나타내는 값이다. 그대로 받으면 서버가 보낸
        // 잘못된 거리와 미측정이 구분되지 않은 채 객체 목록에 섞인다
        double distance = -1.0;
        if (readMqttFiniteNumber(sourceObject, QStringLiteral("dist"), distance) && distance >= 0.0) {
            parsedObject.distance = distance;
        }

        frame.riskLevel = highestRiskLevel(frame.riskLevel, objectRiskLevel);
        frame.objects.append(std::move(parsedObject));
    }

    return true;
}
