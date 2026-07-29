#pragma once

#include "network/MqttRuntimeConfig.h"
#include "network/MqttTopicHandler.h"

class DeviceStatusTopicHandler final : public MqttTopicHandler {
public:
    explicit DeviceStatusTopicHandler(MqttTopicsConfig config);

    QVector<MqttSubscription> subscriptions() const override;
    bool matchesTopic(const QString& topic) const override;
    bool handle(const QByteArray& payload, const QString& topic, MqttMessageBatch& messages,
                QString& error) const override;

private:
    MqttTopicsConfig config_;
};
