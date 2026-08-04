#pragma once

#include <QElapsedTimer>
#include <QObject>

#include "model/MqttRealtimeData.h"
#include "network/transport/MqttRuntimeConfig.h"

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

    QElapsedTimer clock_;
    RiskFrameData pendingFrame_;
    QTimer* flushTimer_ = nullptr;
    MqttDispatcherConfig config_;
    qint64 latestSourceTimestamp_ = 0;
    qint64 lastArrivalMsec_ = 0;
    qint64 debugWindowStartMsec_ = 0;
    int debugReceivedCount_ = 0;
    int debugCoalescedCount_ = 0;
    int debugDeliveredCount_ = 0;
    bool hasPendingFrame_ = false;
    bool running_ = false;
};
