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

/**
 * 고정소수점 역수의 소수부 비트 수.
 *
 * 픽셀당 정수 나눗셈을 곱셈+시프트로 바꾼다. reciprocal = (1 << shift) / count + 1로 두면
 * (sum * reciprocal) >> shift 의 오차는 sum / (1 << shift)보다 작다. 몫의 소수부는 최대
 * (count - 1) / count이므로, sum / (1 << shift) < 1 / count 이면 내림 결과가 나눗셈과 같다.
 * sum <= 255 * count이므로 조건은 (1 << shift) > 255 * count^2이고, count 상한이
 * 2 * maximumRadius + 1 = 4097이라 255 * 4097^2 = 4.3e9 < 2^40이다. 즉 이 시프트에서는
 * 설정이 허용하는 모든 반경에서 나눗셈과 결과가 완전히 같다.
 *
 * 이 값을 줄이면 그 등식이 깨진다. blur_processor_nv12_check가 실제로 확인한다.
 */
constexpr int blurReciprocalShift = 40;

/**
 * @brief 프레임 사이에 재사용하는 블러 작업 버퍼입니다.
 *
 * @details 매 프레임 할당하지 않도록 처리기가 들고 다닌다. reciprocals는 픽셀당 정수 나눗셈을
 *          곱셈+시프트로 바꾸기 위한 고정소수점 역수 표이고, 필요한 크기보다 작을 때만 다시
 *          만든다(영역이 작아지는 것만으로 다시 만들지 않는다).
 */
struct BlurScratch {
    /// 가로 패스가 만든 중간값
    std::vector<guint8> samples;
    /// 창 크기(count) -> (1 << 40) / count + 1
    std::vector<quint64> reciprocals;
};

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
    QVector<QRectF> regionsFor(qint64 sourceTimestamp) const;

    BlurProcessorConfig config_;
    QElapsedTimer metadataClock_;
    VideoUtcClockMapper utcClockMapper_;
    mutable QMutex mutex_;
    QVector<BlurFrameData> history_;
    qint64 latestSourceTimestamp_ = 0;
    qint64 lastMetadataArrivalMsec_ = 0;
    BlurScratch scratch_;
    std::atomic_int channelIndex_{-1};
    std::atomic_bool faceEnabled_{true};
    std::atomic_bool licensePlateEnabled_{true};
    std::atomic<qint64> lastApplyLogMsec_{0};
};
