#pragma once

#include <QObject>
#include <QTimer>
#include <memory>

#include "network/MqttConnectionConfig.h"
#include "network/MqttTransport.h"

class QMqttClient;

class QtMqttTransport final : public QObject, public MqttTransport {
public:
    explicit QtMqttTransport(MqttConnectionConfig config);
    ~QtMqttTransport() override;

    void setCallbacks(MqttTransportCallbacks callbacks) override;
    void start() override;
    void stop() override;
    bool subscribe(const MqttSubscription& subscription) override;

private:
    void connectToBroker();
    void scheduleReconnect();
    void reportError(QString detail);

    MqttConnectionConfig config_;
    MqttTransportCallbacks callbacks_;
    QTimer reconnectTimer_;
    std::unique_ptr<QMqttClient> client_;
    bool stopping_ = false;
};
