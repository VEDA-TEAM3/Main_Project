#pragma once

#include "network/MqttTopicHandler.h"

class BlurTopicHandler final : public MqttTopicHandler {
public:
    explicit BlurTopicHandler(MqttSubscription subscription);

    QVector<MqttSubscription> subscriptions() const override;
    bool matchesTopic(const QString& topic) const override;
    bool handle(const QByteArray& payload, const QString& topic, MqttMessageBatch& messages,
                QString& error) const override;
    bool logPayload() const override;

private:
    MqttSubscription subscription_;
};
