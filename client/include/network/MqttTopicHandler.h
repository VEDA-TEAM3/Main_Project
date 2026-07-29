#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include "model/DeviceStatusReport.h"
#include "model/MqttRealtimeData.h"
#include "network/MqttSubscription.h"

struct MqttMessageBatch {
    QVector<DeviceStatusReport> reports;
    QVector<RiskFrameData> riskFrames;
    QVector<BlurFrameData> blurFrames;
    QVector<CentralEventData> centralEvents;
};

class MqttTopicHandler {
public:
    virtual ~MqttTopicHandler() = default;

    virtual QVector<MqttSubscription> subscriptions() const = 0;
    virtual bool matchesTopic(const QString& topic) const = 0;
    virtual bool handle(const QByteArray& payload, const QString& topic, MqttMessageBatch& messages,
                        QString& error) const = 0;
    virtual bool logPayload() const { return true; }
};
