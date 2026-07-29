#pragma once

#include <QMap>
#include <QMutex>

#include "network/BlurFrameBuffer.h"

class LatestBlurFrameBuffer final : public BlurFrameBuffer {
public:
    bool submit(BlurFrameData frame) override;
    QVector<BlurFrameData> takeLatestFrames() override;
    void removeChannel(int channelIndex) override;
    void cancelPendingDelivery() override;
    void clear() override;
    quint64 takeCoalescedFrameCount() override;

private:
    QMutex mutex_;
    QMap<int, BlurFrameData> latestFrames_;
    quint64 coalescedFrameCount_ = 0;
    bool deliveryPending_ = false;
};
