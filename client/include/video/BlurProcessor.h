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

    /** @brief 프레임 시각과 실제로 고른 metadata 시각의 어긋남을 모은 진단값 */
    struct MatchStatistics {
        qint64 meanDeltaMsec = 0;  ///< metadata ts - 프레임 시각의 평균. 양수면 metadata가 더 미래다
        qint64 matchedCount = 0;   ///< 맞는 metadata를 찾은 프레임 수
        qint64 missedCount = 0;    ///< 못 찾아 블러를 건너뛴 프레임 수
    };

private:
    QVector<QRectF> regionsFor(qint64 sourceTimestamp) const;
    MatchStatistics takeMatchStatistics() const;

    BlurProcessorConfig config_;
    QElapsedTimer metadataClock_;
    VideoUtcClockMapper utcClockMapper_;
    mutable QMutex mutex_;
    QVector<BlurFrameData> history_;
    qint64 latestSourceTimestamp_ = 0;
    qint64 lastMetadataArrivalMsec_ = 0;
    // 아래 셋은 regionsFor()가 이미 잡고 있는 mutex_로 보호한다
    mutable qint64 matchDeltaSumMsec_ = 0;
    mutable qint64 matchedFrameCount_ = 0;
    mutable qint64 missedFrameCount_ = 0;
    std::vector<guint8> scratch_;
    std::atomic_int channelIndex_{-1};
    std::atomic_bool faceEnabled_{true};
    std::atomic_bool licensePlateEnabled_{true};
    std::atomic<qint64> lastApplyLogMsec_{0};
};
