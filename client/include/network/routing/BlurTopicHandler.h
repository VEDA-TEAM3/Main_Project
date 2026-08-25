#pragma once

#include "network/routing/MqttTopicHandler.h"

class BlurTopicHandler final : public MqttTopicHandler {
public:
    BlurTopicHandler(MqttSubscription subscription, int channelCount);

    QVector<MqttSubscription> subscriptions() const override;
    bool matchesTopic(const QString& topic) const override;
    bool handle(const QByteArray& payload, const QString& topic, MqttMessageBatch& messages,
                QString& error) const override;
    bool logPayload() const override;

private:
    MqttSubscription subscription_;
    int channelCount_ = 0;
};
