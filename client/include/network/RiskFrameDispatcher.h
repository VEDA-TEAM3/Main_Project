#pragma once

#include <QObject>

#include "model/MqttRealtimeData.h"
#include "network/MqttRuntimeConfig.h"

class QTimer;

class RiskFrameDispatcher final : public QObject {
    Q_OBJECT

public:
    explicit RiskFrameDispatcher(MqttDispatcherConfig config, QObject* parent = nullptr);

    void start();
    void stop();
    void reset();
    void submitFrame(RiskFrameData frame);

signals:
    void frameReady(RiskFrameData frame);

private:
    void flushPendingFrame();

    RiskFrameData pendingFrame_;
    QTimer* flushTimer_ = nullptr;
    MqttDispatcherConfig config_;
    qint64 latestSourceTimestamp_ = 0;
    qint64 lastArrivalMsec_ = 0;
    bool hasPendingFrame_ = false;
    bool running_ = false;
};
