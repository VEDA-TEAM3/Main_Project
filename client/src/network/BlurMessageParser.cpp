#include "network/BlurMessageParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <utility>

namespace {
constexpr int blurDeviceChannelCount = 4;
constexpr int blurProtocolVersion = 1;

/**
 * @brief        JSON 필드에서 손실 없는 정수 값을 읽습니다.
 * @param object JSON 객체
 * @param name   필드 이름
 * @param value  읽은 정수
 * @return       유효한 정수이면 true
 */
bool readBlurInteger(const QJsonObject& object, const QString& name, qint64& value) {
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

/**
 * @brief        JSON 필드에서 유한한 실수 값을 읽습니다.
 * @param object JSON 객체
 * @param name   필드 이름
 * @param value  읽은 실수
 * @return       유효한 실수이면 true
 */
bool readBlurFiniteNumber(const QJsonObject& object, const QString& name, double& value) {
    const QJsonValue jsonValue = object.value(name);
    if (!jsonValue.isDouble() || !std::isfinite(jsonValue.toDouble())) {
        return false;
    }

    value = jsonValue.toDouble();
    return true;
}
}  // namespace

/**
 * @brief         블러 메타데이터 payload를 내부 프레임 모델로 변환합니다.
 * @param payload MQTT JSON payload
 * @param topic   수신 토픽
 * @param topicWireChannel 토픽 wildcard에서 검증한 채널 인덱스
 * @param frame   변환된 블러 프레임
 * @param error   실패 원인
 * @return        변환에 성공하면 true
 */
bool BlurMessageParser::parse(const QByteArray& payload, const QString& topic, int topicWireChannel,
                              BlurFrameData& frame, QString& error) {
    if (topicWireChannel < 0 || topicWireChannel >= blurDeviceChannelCount) {
        error = QStringLiteral("Invalid blur topic: %1").arg(topic);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("Invalid blur JSON on %1: %2").arg(topic, parseError.errorString());
        return false;
    }

    const QJsonObject object = document.object();
    qint64 version = 0;
    qint64 timestamp = 0;
    qint64 payloadChannel = 0;
    if (!readBlurInteger(object, QStringLiteral("v"), version) || version != blurProtocolVersion ||
        !readBlurInteger(object, QStringLiteral("ts"), timestamp) || timestamp <= 0 ||
        !readBlurInteger(object, QStringLiteral("ch"), payloadChannel) || payloadChannel < 0 ||
        payloadChannel >= blurDeviceChannelCount) {
        error = QStringLiteral("Invalid v, ts or ch field on %1").arg(topic);
        return false;
    }

    if (payloadChannel != topicWireChannel) {
        error = QStringLiteral("Blur topic/payload channel mismatch on %1").arg(topic);
        return false;
    }

    const QJsonValue blursValue = object.value(QStringLiteral("blurs"));
    if (!blursValue.isArray()) {
        error = QStringLiteral("Missing blurs array on %1").arg(topic);
        return false;
    }

    frame = {};
    frame.channelIndex = topicWireChannel;
    frame.sourceTimestamp = timestamp;

    const QJsonArray blurs = blursValue.toArray();
    frame.regions.reserve(blurs.size());
    for (const QJsonValue& blurValue : blurs) {
        if (!blurValue.isObject()) {
            error = QStringLiteral("Blur targets must be JSON objects on %1").arg(topic);
            return false;
        }

        const QJsonObject blur = blurValue.toObject();
        const QJsonValue classValue = blur.value(QStringLiteral("cls"));
        const QJsonValue boxValue = blur.value(QStringLiteral("box"));
        qint64 id = 0;
        if (!readBlurInteger(blur, QStringLiteral("id"), id) || !classValue.isString() || !boxValue.isObject()) {
            error = QStringLiteral("Invalid blur target fields on %1").arg(topic);
            return false;
        }

        const QString objectClass = classValue.toString();
        if (objectClass != QStringLiteral("Head") && objectClass != QStringLiteral("LicensePlate")) {
            error = QStringLiteral("Unsupported blur class on %1: %2").arg(topic, objectClass);
            return false;
        }

        const QJsonObject box = boxValue.toObject();
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        if (!readBlurFiniteNumber(box, QStringLiteral("l"), left) ||
            !readBlurFiniteNumber(box, QStringLiteral("t"), top) ||
            !readBlurFiniteNumber(box, QStringLiteral("r"), right) ||
            !readBlurFiniteNumber(box, QStringLiteral("b"), bottom) || left >= right || top >= bottom) {
            error = QStringLiteral("Invalid blur box on %1").arg(topic);
            return false;
        }

        const double clippedLeft = qBound(0.0, left, 1.0);
        const double clippedTop = qBound(0.0, top, 1.0);
        const double clippedRight = qBound(0.0, right, 1.0);
        const double clippedBottom = qBound(0.0, bottom, 1.0);
        if (clippedLeft >= clippedRight || clippedTop >= clippedBottom) {
            continue;
        }

        BlurRegionData region;
        region.id = id;
        region.targetType = objectClass == QStringLiteral("Head") ? BlurTargetType::Face : BlurTargetType::LicensePlate;
        region.normalizedBox = QRectF(QPointF(clippedLeft, clippedTop), QPointF(clippedRight, clippedBottom));
        frame.regions.append(std::move(region));
    }

    return true;
}
