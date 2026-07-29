#pragma once

#include <QVector>
#include <memory>

#include "network/MqttTopicHandler.h"

struct MqttRouteResult {
    bool handled = false;
    bool successful = false;
    bool logPayload = true;
    MqttMessageBatch messages;
    QString error;
};

class MqttMessageRouter final {
public:
    explicit MqttMessageRouter(QVector<std::shared_ptr<MqttTopicHandler>> handlers);

    QVector<MqttSubscription> subscriptions() const;
    MqttRouteResult route(const QByteArray& payload, const QString& topic) const;

private:
    QVector<std::shared_ptr<MqttTopicHandler>> handlers_;
};
