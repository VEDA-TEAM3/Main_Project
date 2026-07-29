#include "network/RiskTopicHandler.h"

#include <utility>

#include "network/MqttTopicFilter.h"
#include "network/RiskMessageParser.h"

RiskTopicHandler::RiskTopicHandler(MqttSubscription subscription) : subscription_(std::move(subscription)) {}

/** @brief 통합 위험 프레임 구독을 반환합니다. */
QVector<MqttSubscription> RiskTopicHandler::subscriptions() const { return {subscription_}; }

/** @brief 수신 토픽이 통합 위험 계약에 속하는지 확인합니다. */
bool RiskTopicHandler::matchesTopic(const QString& topic) const {
    return MqttTopicFilter::matches(subscription_.topicFilter, topic);
}

/** @brief 위험 payload를 지도 갱신기가 소비할 도메인 프레임으로 변환합니다. */
bool RiskTopicHandler::handle(const QByteArray& payload, const QString& topic, MqttMessageBatch& messages,
                              QString& error) const {
    RiskFrameData frame;
    if (!RiskMessageParser::parse(payload, topic, frame, error)) {
        return false;
    }

    messages.riskFrames.append(std::move(frame));
    return true;
}

/** @brief 고빈도 위험 payload의 원문 로그를 생략합니다. */
bool RiskTopicHandler::logPayload() const { return false; }
