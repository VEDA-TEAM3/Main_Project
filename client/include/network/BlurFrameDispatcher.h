#pragma once

#include <QMap>
#include <QObject>
#include <memory>

#include "model/MqttRealtimeData.h"
#include "network/MqttRuntimeConfig.h"

class QTimer;
class BlurFrameBuffer;

class BlurFrameDispatcher final : public QObject {
    Q_OBJECT

public:
    explicit BlurFrameDispatcher(MqttDispatcherConfig config, std::shared_ptr<BlurFrameBuffer> frameBuffer,
                                 QObject* parent = nullptr);

    void start();
    void stop();
    void submitFrame(BlurFrameData frame);

signals:
    void frameReady(BlurFrameData frame);

private:
    void flushPendingFrames();

    std::shared_ptr<BlurFrameBuffer> frameBuffer_;
    QMap<int, qint64> latestSourceTimes_;
    QMap<int, qint64> lastArrivalTimes_;
    QTimer* flushTimer_ = nullptr;
    MqttDispatcherConfig config_;
    qint64 lastStatisticsLogMsec_ = 0;
    quint64 deliveredFrameCount_ = 0;
    bool running_ = false;
};
