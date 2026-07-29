#include "network/RiskMessageParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <utility>

namespace {
constexpr int riskProtocolVersion = 1;

bool readInteger(const QJsonObject& object, const QString& name, qint64& value) {
    const QJsonValue jsonValue = object.value(name);
    if (!jsonValue.isDouble()) {
        return false;
    }

    const qint64 integerValue = jsonValue.toInteger();
    if (static_cast<double>(integerValue) != jsonValue.toDouble()) {
        return false;
    }

    value = integerValue;
    return true;
}

bool readFiniteNumber(const QJsonObject& object, const QString& name, double& value) {
    const QJsonValue jsonValue = object.value(name);
    if (!jsonValue.isDouble() || !std::isfinite(jsonValue.toDouble())) {
        return false;
    }

    value = jsonValue.toDouble();
    return true;
}

bool parseRiskLevel(const QJsonValue& value, DigitalTwinRiskLevel& riskLevel) {
    if (!value.isString()) {
        return false;
    }

    const QString text = value.toString();
    if (text == QStringLiteral("None")) {
        riskLevel = DigitalTwinRiskLevel::Normal;
        return true;
    }
    if (text == QStringLiteral("Warning")) {
        riskLevel = DigitalTwinRiskLevel::Warning;
        return true;
    }
    if (text == QStringLiteral("Danger")) {
        riskLevel = DigitalTwinRiskLevel::Danger;
        return true;
    }
    return false;
}
}  // namespace

/**
 * @brief         공유 계약의 RiskFrame을 지도 입력 프레임으로 변환합니다.
 * @param payload MQTT JSON payload
 * @param topic   수신 토픽
 * @param frame   변환된 통합 지도 프레임
 * @param error   검증 실패 원인
 * @return        계약 검증과 변환에 성공하면 true
 */
bool RiskMessageParser::parse(const QByteArray& payload, const QString& topic, RiskFrameData& frame, QString& error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("Invalid RiskFrame JSON on %1: %2").arg(topic, parseError.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    qint64 version = 0;
    qint64 timestamp = 0;
    DigitalTwinRiskLevel frameRiskLevel = DigitalTwinRiskLevel::Normal;
    if (!readInteger(root, QStringLiteral("v"), version) || version != riskProtocolVersion ||
        !readInteger(root, QStringLiteral("ts"), timestamp) || timestamp <= 0 ||
        !parseRiskLevel(root.value(QStringLiteral("level")), frameRiskLevel)) {
        error = QStringLiteral("Invalid v, ts or level field on %1").arg(topic);
        return false;
    }

    const QJsonValue objectsValue = root.value(QStringLiteral("objects"));
    if (!objectsValue.isArray()) {
        error = QStringLiteral("Missing objects array on %1").arg(topic);
        return false;
    }

    frame = {};
    frame.sourceTimestamp = timestamp;
    frame.riskLevel = frameRiskLevel;

    const QJsonArray objects = objectsValue.toArray();
    frame.objects.reserve(objects.size());
    for (const QJsonValue& objectValue : objects) {
        if (!objectValue.isObject()) {
            error = QStringLiteral("RiskFrame objects must be JSON objects on %1").arg(topic);
            return false;
        }

        const QJsonObject sourceObject = objectValue.toObject();
        const QJsonValue classValue = sourceObject.value(QStringLiteral("cls"));
        const QJsonValue positionValue = sourceObject.value(QStringLiteral("pos"));
        qint64 objectId = 0;
        qint64 nearestId = 0;
        double distance = 0.0;
        DigitalTwinRiskLevel objectRiskLevel = DigitalTwinRiskLevel::Normal;
        if (!readInteger(sourceObject, QStringLiteral("gid"), objectId) || objectId <= 0 || !classValue.isString() ||
            !positionValue.isObject() ||
            !parseRiskLevel(sourceObject.value(QStringLiteral("level")), objectRiskLevel) ||
            !readInteger(sourceObject, QStringLiteral("nearest"), nearestId) || nearestId < 0 ||
            !readFiniteNumber(sourceObject, QStringLiteral("dist"), distance)) {
            error = QStringLiteral("Invalid RiskObject fields on %1").arg(topic);
            return false;
        }

        const QString objectClass = classValue.toString();
        if (objectClass != QStringLiteral("Human") && objectClass != QStringLiteral("Vehicle")) {
            error = QStringLiteral("Unsupported RiskObject class on %1: %2").arg(topic, objectClass);
            return false;
        }

        const QJsonObject position = positionValue.toObject();
        double x = 0.0;
        double y = 0.0;
        if (!readFiniteNumber(position, QStringLiteral("x"), x) ||
            !readFiniteNumber(position, QStringLiteral("y"), y)) {
            error = QStringLiteral("Invalid RiskObject world position on %1").arg(topic);
            return false;
        }

        RiskObjectData parsedObject;
        parsedObject.globalId = objectId;
        parsedObject.objectClass = objectClass;
        parsedObject.worldPosition = QPointF(x, y);
        parsedObject.riskLevel = objectRiskLevel;
        parsedObject.nearestId = nearestId;
        parsedObject.distance = distance;
        frame.objects.append(std::move(parsedObject));
    }

    return true;
}
