#include "video/VideoUtcClockMapper.h"

#include <QDateTime>
#include <QMutexLocker>

namespace {
constexpr qint64 nanosecondsPerMillisecond = 1000LL * 1000LL;
constexpr qint64 ntpToUnixEpochSeconds = 2208988800LL;
constexpr qint64 nanosecondsPerSecond = 1000LL * 1000LL * 1000LL;
constexpr qint64 ntpToUnixEpochNanoseconds = ntpToUnixEpochSeconds * nanosecondsPerSecond;
constexpr qint64 maximumAnchorDiscontinuityMsec = 2000;

/**
 * @brief   reference timestamp caps의 첫 구조체 이름을 반환합니다.
 * @param   reference GstReferenceTimestampMeta의 기준 caps
 * @return  caps 구조체 이름이며 확인할 수 없으면 빈 문자열
 */
const char* referenceName(const GstCaps* reference) {
    if (!reference || gst_caps_is_empty(reference) || gst_caps_get_size(reference) == 0) {
        return nullptr;
    }

    const GstStructure* structure = gst_caps_get_structure(reference, 0);
    return structure ? gst_structure_get_name(structure) : nullptr;
}
}  // namespace

/**
 * @brief RTSP 재연결 시 PTS와 UTC 사이의 기존 기준점을 초기화합니다.
 */
void VideoUtcClockMapper::reset() {
    QMutexLocker locker(&mutex_);
    anchorPtsMsec_ = 0;
    anchorUtcMsec_ = 0;
    anchorReady_ = false;
    senderClockReady_ = false;
}

/**
 * @brief        RTP 출력 buffer에서 RTCP 기준 시각 또는 최초 도착 기준점을 학습합니다.
 * @param buffer rtspsrc 내부 jitterbuffer를 통과한 영상 buffer
 */
void VideoUtcClockMapper::observe(const GstBuffer* buffer) {
    const std::optional<qint64> pts = ptsMsec(buffer);
    if (!pts.has_value()) {
        return;
    }

    const std::optional<qint64> referenceUtc = referenceUtcMsec(buffer);
    QMutexLocker locker(&mutex_);

    if (referenceUtc.has_value()) {
        updateAnchor(*pts, *referenceUtc, true);
        return;
    }

    if (!anchorReady_) {
        updateAnchor(*pts, QDateTime::currentMSecsSinceEpoch(), false);
    }
}

/**
 * @brief        디코딩된 영상 buffer의 PTS를 절대 UTC millisecond로 변환합니다.
 * @param buffer qtblur에 전달된 디코딩 영상 buffer
 * @return       계산된 UTC와 RTCP sender clock 사용 여부
 */
std::optional<VideoUtcTimestamp> VideoUtcClockMapper::timestampFor(const GstBuffer* buffer) {
    const std::optional<qint64> pts = ptsMsec(buffer);
    if (!pts.has_value()) {
        return std::nullopt;
    }

    const std::optional<qint64> referenceUtc = referenceUtcMsec(buffer);
    QMutexLocker locker(&mutex_);

    if (referenceUtc.has_value()) {
        updateAnchor(*pts, *referenceUtc, true);
        return VideoUtcTimestamp{*referenceUtc, true};
    }

    if (!anchorReady_) {
        updateAnchor(*pts, QDateTime::currentMSecsSinceEpoch(), false);
    }

    return VideoUtcTimestamp{anchorUtcMsec_ + (*pts - anchorPtsMsec_), senderClockReady_};
}

/**
 * @brief        GstReferenceTimestampMeta를 Unix epoch millisecond로 변환합니다.
 * @param buffer 검사할 GStreamer buffer
 * @return       지원되는 UTC reference meta가 있으면 Unix epoch millisecond
 */
std::optional<qint64> VideoUtcClockMapper::referenceUtcMsec(const GstBuffer* buffer) {
    if (!buffer) {
        return std::nullopt;
    }

    const auto* meta = gst_buffer_get_reference_timestamp_meta(const_cast<GstBuffer*>(buffer), nullptr);
    if (!meta || !GST_CLOCK_TIME_IS_VALID(meta->timestamp)) {
        return std::nullopt;
    }

    const char* name = referenceName(meta->reference);
    if (!name) {
        return std::nullopt;
    }

    const qint64 timestampNanoseconds = static_cast<qint64>(meta->timestamp);
    if (g_str_equal(name, "timestamp/x-unix")) {
        return timestampNanoseconds / nanosecondsPerMillisecond;
    }

    if (g_str_equal(name, "timestamp/x-ntp") && timestampNanoseconds >= ntpToUnixEpochNanoseconds) {
        return (timestampNanoseconds - ntpToUnixEpochNanoseconds) / nanosecondsPerMillisecond;
    }

    return std::nullopt;
}

/**
 * @brief        buffer PTS를 millisecond 단위로 변환합니다.
 * @param buffer 검사할 GStreamer buffer
 * @return       유효한 PTS가 있으면 millisecond 값
 */
std::optional<qint64> VideoUtcClockMapper::ptsMsec(const GstBuffer* buffer) {
    if (!buffer || !GST_BUFFER_PTS_IS_VALID(buffer)) {
        return std::nullopt;
    }

    return static_cast<qint64>(GST_BUFFER_PTS(buffer) / GST_MSECOND);
}

/**
 * @brief             PTS와 절대 UTC의 대응 기준점을 갱신합니다.
 * @param ptsMsec     GStreamer PTS millisecond
 * @param utcMsec     대응하는 절대 UTC millisecond
 * @param senderClock RTCP 또는 명시적 sender clock에서 얻은 값인지 여부
 */
void VideoUtcClockMapper::updateAnchor(qint64 ptsMsec, qint64 utcMsec, bool senderClock) {
    if (anchorReady_ && senderClockReady_ && senderClock) {
        const qint64 predictedUtc = anchorUtcMsec_ + (ptsMsec - anchorPtsMsec_);
        if (qAbs(predictedUtc - utcMsec) <= maximumAnchorDiscontinuityMsec) {
            anchorPtsMsec_ = ptsMsec;
            anchorUtcMsec_ = utcMsec;
            return;
        }
    }

    if (!anchorReady_ || senderClock || !senderClockReady_) {
        anchorPtsMsec_ = ptsMsec;
        anchorUtcMsec_ = utcMsec;
        anchorReady_ = true;
        senderClockReady_ = senderClock;
    }
}
