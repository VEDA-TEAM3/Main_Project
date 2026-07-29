#include "network/MqttMessageRouter.h"

#include <QMap>
#include <utility>

/**
 * @brief          등록 순서에 따라 토픽 handler를 선택하는 router를 생성합니다.
 * @param handlers 서로 겹치지 않는 MQTT 토픽 handler 목록
 */
MqttMessageRouter::MqttMessageRouter(QVector<std::shared_ptr<MqttTopicHandler>> handlers)
    : handlers_(std::move(handlers)) {}

/**
 * @brief   모든 handler가 요구하는 구독을 중복 없이 병합합니다.
 * @return  토픽별 가장 높은 QoS를 적용한 구독 목록
 */
QVector<MqttSubscription> MqttMessageRouter::subscriptions() const {
    QMap<QString, quint8> qosByTopic;
    for (const std::shared_ptr<MqttTopicHandler>& handler : handlers_) {
        if (!handler) {
            continue;
        }

        for (const MqttSubscription& subscription : handler->subscriptions()) {
            qosByTopic[subscription.topicFilter] =
                qMax(qosByTopic.value(subscription.topicFilter, 0), subscription.qos);
        }
    }

    QVector<MqttSubscription> subscriptions;
    subscriptions.reserve(qosByTopic.size());
    for (auto iterator = qosByTopic.cbegin(); iterator != qosByTopic.cend(); ++iterator) {
        subscriptions.append({iterator.key(), iterator.value()});
    }
    return subscriptions;
}

/**
 * @brief         수신 토픽을 담당 handler에 전달하고 도메인 메시지로 변환합니다.
 * @param payload MQTT payload
 * @param topic   실제 수신 토픽
 * @return        처리 여부, 변환 결과 및 오류
 */
MqttRouteResult MqttMessageRouter::route(const QByteArray& payload, const QString& topic) const {
    MqttRouteResult result;
    for (const std::shared_ptr<MqttTopicHandler>& handler : handlers_) {
        if (!handler || !handler->matchesTopic(topic)) {
            continue;
        }

        result.handled = true;
        result.logPayload = handler->logPayload();
        result.successful = handler->handle(payload, topic, result.messages, result.error);
        return result;
    }

    result.error = QStringLiteral("Unsupported MQTT topic: %1").arg(topic);
    return result;
}
