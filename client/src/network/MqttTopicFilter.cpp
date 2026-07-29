#include "network/MqttTopicFilter.h"

#include <QStringList>

/**
 * @brief        MQTT 단일 단계(+) 및 다중 단계(#) wildcard를 사용해 토픽을 비교합니다.
 * @param filter 구독 토픽 필터
 * @param topic  실제 수신 토픽
 * @return       필터와 토픽이 일치하면 true
 */
bool MqttTopicFilter::matches(const QString& filter, const QString& topic) {
    const QStringList filterLevels = filter.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    const QStringList topicLevels = topic.split(QLatin1Char('/'), Qt::KeepEmptyParts);

    for (qsizetype index = 0; index < filterLevels.size(); ++index) {
        const QString& filterLevel = filterLevels[index];
        if (filterLevel == QStringLiteral("#")) {
            return index == filterLevels.size() - 1;
        }
        if (index >= topicLevels.size()) {
            return false;
        }
        if (filterLevel != QStringLiteral("+") && filterLevel != topicLevels[index]) {
            return false;
        }
    }
    return filterLevels.size() == topicLevels.size();
}

/**
 * @brief         필터의 첫 + 위치에서 정수 채널 값을 추출합니다.
 * @param filter  구독 토픽 필터
 * @param topic   실제 수신 토픽
 * @param minimum 허용 최솟값
 * @param maximum 허용 최댓값
 * @return        범위 안의 정수, 실패하면 -1
 */
int MqttTopicFilter::integerWildcardValue(const QString& filter, const QString& topic, int minimum, int maximum) {
    if (!matches(filter, topic)) {
        return -1;
    }

    const QStringList filterLevels = filter.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    const QStringList topicLevels = topic.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    const qsizetype wildcardIndex = filterLevels.indexOf(QStringLiteral("+"));
    if (wildcardIndex < 0 || wildcardIndex >= topicLevels.size()) {
        return -1;
    }

    bool valid = false;
    const int value = topicLevels[wildcardIndex].toInt(&valid);
    return valid && value >= minimum && value <= maximum ? value : -1;
}
