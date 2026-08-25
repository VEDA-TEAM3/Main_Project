#pragma once

#include <gst/video/video-frame.h>

#include <QElapsedTimer>
#include <QMutex>
#include <QRectF>
#include <QVector>
#include <atomic>
#include <optional>
#include <vector>

#include "model/MqttRealtimeData.h"
#include "video/VideoRuntimeConfig.h"
#include "video/VideoUtcClockMapper.h"

class BlurProcessor final {
public:
    explicit BlurProcessor(BlurProcessorConfig config);

    void setTargetsEnabled(bool faceEnabled, bool licensePlateEnabled);

    /** @brief 블러를 적용할 대상이 하나라도 켜져 있는지 확인합니다. */
    bool hasEnabledTargets() const {
        return faceEnabled_.load(std::memory_order_acquire) || licensePlateEnabled_.load(std::memory_order_acquire);
    }
    void submitFrame(BlurFrameData frame);
    void observeVideoBuffer(const GstBuffer* buffer);
    void clear();
    void apply(GstVideoFrame& frame);

private:
    QVector<QRectF> regionsFor(qint64 sourceTimestamp, qint64& metadataLagMsec) const;

    BlurProcessorConfig config_;
    QElapsedTimer metadataClock_;
    VideoUtcClockMapper utcClockMapper_;
    mutable QMutex mutex_;
    QVector<BlurFrameData> history_;
    qint64 latestSourceTimestamp_ = 0;
    qint64 lastMetadataArrivalMsec_ = 0;
    std::atomic_int channelIndex_{-1};
    std::atomic_bool faceEnabled_{true};
    std::atomic_bool licensePlateEnabled_{true};
    std::atomic<qint64> lastApplyLogMsec_{0};
};
