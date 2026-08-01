#pragma once

#include <QObject>

#include "model/DeviceStatusReport.h"
#include "model/MqttRealtimeData.h"

class DeviceStatusGateway : public QObject {
    Q_OBJECT

public:
    explicit DeviceStatusGateway(QObject* parent = nullptr);
    ~DeviceStatusGateway() override;

    virtual void start() = 0;
    virtual void stop() = 0;

signals:
    void reportReceived(DeviceStatusReport report);
    void riskFrameReceived(RiskFrameData frame);
    void blurFrameReceived(BlurFrameData frame);
    void centralEventReceived(CentralEventData event);
    void brokerConnectionChanged(bool connected);
};
