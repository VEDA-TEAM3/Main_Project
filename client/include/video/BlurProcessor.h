#pragma once

#include <gst/video/video-frame.h>

#include <QMutex>
#include <QRectF>
#include <QVector>
#include <atomic>
#include <vector>

#include "model/MqttRealtimeData.h"
#include "video/VideoRuntimeConfig.h"
#include "video/VideoUtcClockMapper.h"

class BlurProcessor final {
public:
    explicit BlurProcessor(BlurProcessorConfig config);

    void setTargetsEnabled(bool faceEnabled, bool licensePlateEnabled);
    void submitFrame(BlurFrameData frame);
    void observeVideoBuffer(const GstBuffer* buffer);
    void clear();
    void apply(GstVideoFrame& frame);

private:
    QVector<QRectF> regionsFor(qint64 sourceTimestamp) const;

    BlurProcessorConfig config_;
    VideoUtcClockMapper utcClockMapper_;
    mutable QMutex mutex_;
    QVector<BlurFrameData> history_;
    qint64 latestSourceTimestamp_ = 0;
    qint64 lastMetadataArrivalMsec_ = 0;
    std::vector<guint8> scratch_;
    std::atomic_int channelIndex_{-1};
    std::atomic_bool faceEnabled_{true};
    std::atomic_bool licensePlateEnabled_{true};
    std::atomic<qint64> lastApplyLogMsec_{0};
};
