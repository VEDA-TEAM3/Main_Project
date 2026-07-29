#pragma once

#include <gst/gst.h>

#include <QObject>
#include <QString>

#include "model/MqttRealtimeData.h"

class QThread;

class StreamReceiver : public QObject {
    Q_OBJECT

public:
    explicit StreamReceiver(QObject* parent = nullptr) : QObject(parent) {}
    ~StreamReceiver() override = default;

    virtual void setUrl(const QString& url) = 0;
    virtual void setBlurTargetsEnabled(bool faceEnabled, bool licensePlateEnabled) = 0;
    virtual void setBlurFrame(BlurFrameData frame) = 0;
    virtual void moveInternalObjectsToThread(QThread* thread) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;

signals:
    void statusChanged(const QString& status);
    void errorOccurred(const QString& error);
    void loadingChanged(bool loading);
    void streamDataReceived();
    void firstFrameReceived();
};
