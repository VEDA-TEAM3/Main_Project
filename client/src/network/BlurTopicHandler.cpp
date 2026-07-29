#include "network/BlurTopicHandler.h"

#include <utility>

#include "network/BlurMessageParser.h"
#include "network/MqttTopicFilter.h"

BlurTopicHandler::BlurTopicHandler(MqttSubscription subscription) : subscription_(std::move(subscription)) {}

/** @brief 채널별 블러 메타데이터 구독을 반환합니다. */
QVector<MqttSubscription> BlurTopicHandler::subscriptions() const { return {subscription_}; }

/** @brief 수신 토픽이 블러 계약에 속하는지 확인합니다. */
bool BlurTopicHandler::matchesTopic(const QString& topic) const {
    return MqttTopicFilter::matches(subscription_.topicFilter, topic);
}

/** @brief 블러 payload를 지연 병합기가 소비할 도메인 프레임으로 변환합니다. */
bool BlurTopicHandler::handle(const QByteArray& payload, const QString& topic, MqttMessageBatch& messages,
                              QString& error) const {
    BlurFrameData frame;
    const int channelIndex = MqttTopicFilter::integerWildcardValue(subscription_.topicFilter, topic, 0, 3);
    if (!BlurMessageParser::parse(payload, topic, channelIndex, frame, error)) {
        return false;
    }

    messages.blurFrames.append(std::move(frame));
    return true;
}

/** @brief 고빈도 블러 payload의 원문 로그를 생략합니다. */
bool BlurTopicHandler::logPayload() const { return false; }
