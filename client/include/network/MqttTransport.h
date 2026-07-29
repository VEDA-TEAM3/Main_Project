#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

#include "network/MqttSubscription.h"

struct MqttTransportCallbacks {
    std::function<void(bool)> connectionChanged;
    std::function<void(const QByteArray&, const QString&)> messageReceived;
    std::function<void(QString)> errorOccurred;
};

class MqttTransport {
public:
    virtual ~MqttTransport() = default;

    virtual void setCallbacks(MqttTransportCallbacks callbacks) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool subscribe(const MqttSubscription& subscription) = 0;
};
