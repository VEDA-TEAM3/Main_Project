#pragma once

#include <QMap>
#include <QMutex>
#include <QQueue>

#include "network/realtime/BlurFrameBuffer.h"

class LatestBlurFrameBuffer final : public BlurFrameBuffer {
public:
    bool submit(BlurFrameData frame) override;
    QVector<BlurFrameData> takeLatestFrames() override;
    void removeChannel(int channelIndex) override;
    void cancelPendingDelivery() override;
    void clear() override;
    quint64 takeCoalescedFrameCount() override;

private:
    static constexpr qsizetype maximumPendingFramesPerChannel = 32;

    QMutex mutex_;
    QMap<int, QQueue<BlurFrameData>> pendingFrames_;
    quint64 coalescedFrameCount_ = 0;
    bool deliveryPending_ = false;
};
