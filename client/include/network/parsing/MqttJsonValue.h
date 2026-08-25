#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QtGlobal>
#include <cmath>

/**
 * @brief MQTT payload에서 수를 안전하게 꺼내는 공통 리더.
 *
 * @details Risk 파서와 Blur 파서가 같은 함수를 각자 들고 있었습니다. 한쪽만 검사를 강화하면
 *          다른 토픽으로는 거르려던 값이 그대로 들어옵니다. 두 파서가 같은 계약을 쓰도록
 *          한곳에 둡니다.
 */

/**
 * @brief        JSON 필드에서 손실 없는 정수 값을 읽습니다.
 * @param object JSON 객체
 * @param name   필드 이름
 * @param value  읽은 정수
 * @return       유효한 정수이면 true
 *
 * @details JSON 수는 double이므로 2^53을 넘는 정수나 1.5 같은 값이 조용히 반올림됩니다.
 *          되돌린 double이 원본과 다르면 정수가 아니었던 것으로 보고 거부합니다.
 */
inline bool readMqttInteger(const QJsonObject& object, const QString& name, qint64& value) {
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
 *
 * @details NaN과 무한대를 거부합니다. 좌표나 상자 크기로 들어오면 이후 비교가 전부 거짓이 되어
 *          경계 검사와 정렬을 소리 없이 통과합니다.
 */
inline bool readMqttFiniteNumber(const QJsonObject& object, const QString& name, double& value) {
    const QJsonValue jsonValue = object.value(name);
    if (!jsonValue.isDouble() || !std::isfinite(jsonValue.toDouble())) {
        return false;
    }

    value = jsonValue.toDouble();
    return true;
}
