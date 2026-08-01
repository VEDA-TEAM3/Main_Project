#pragma once

#include <QVector>
#include <QtGlobal>

#include "model/MqttRealtimeData.h"

class BlurFrameBuffer {
public:
    virtual ~BlurFrameBuffer() = default;

    virtual bool submit(BlurFrameData frame) = 0;
    virtual QVector<BlurFrameData> takeLatestFrames() = 0;
    virtual void removeChannel(int channelIndex) = 0;
    virtual void cancelPendingDelivery() = 0;
    virtual void clear() = 0;
    virtual quint64 takeCoalescedFrameCount() = 0;
};
