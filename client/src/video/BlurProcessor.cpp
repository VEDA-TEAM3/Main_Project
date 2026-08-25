#include "video/BlurProcessor.h"

#include <gst/video/video-frame.h>

#include <QDateTime>
#include <QDebug>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <utility>

namespace {
/** @brief 설정 로더가 허용하는 blur 반경의 상한 */
constexpr int maximumSupportedRadius = 2048;

/**
 * @brief        실수 픽셀 좌표를 내림한 뒤 영상 범위로 제한합니다.
 * @param value  실수 픽셀 좌표
 * @param extent 영상 축 길이
 * @return       영상 범위로 제한한 픽셀 좌표
 */
int boundedFloorPixel(double value, int extent) {
    int pixel = static_cast<int>(value);
    if (static_cast<double>(pixel) > value) {
        --pixel;
    }
    return qBound(0, pixel, extent);
}

/**
 * @brief        실수 픽셀 좌표를 올림한 뒤 영상 범위로 제한합니다.
 * @param value  실수 픽셀 좌표
 * @param extent 영상 축 길이
 * @return       영상 범위로 제한한 픽셀 좌표
 */
int boundedCeilPixel(double value, int extent) {
    int pixel = static_cast<int>(value);
    if (static_cast<double>(pixel) < value) {
        ++pixel;
    }
    return qBound(0, pixel, extent);
}

/**
 * @brief            정규화 좌표를 내림 방향 픽셀 좌표로 변환합니다.
 * @param normalized 정규화 좌표
 * @param extent     영상 축 길이
 * @return           영상 범위로 제한한 픽셀 좌표
 */
int normalizedFloorPixel(double normalized, int extent) {
    return boundedFloorPixel(normalized * static_cast<double>(extent), extent);
}

/**
 * @brief            정규화 좌표를 올림 방향 픽셀 좌표로 변환합니다.
 * @param normalized 정규화 좌표
 * @param extent     영상 축 길이
 * @return           영상 범위로 제한한 픽셀 좌표
 */
int normalizedCeilPixel(double normalized, int extent) {
    return boundedCeilPixel(normalized * static_cast<double>(extent), extent);
}

/**
 * @brief         이동 중인 객체의 두 블러 영역 사이를 지정 비율로 보간합니다.
 * @param first   이전 메타데이터의 영역
 * @param second  다음 메타데이터의 영역
 * @param ratio   0.0부터 1.0 사이의 보간 비율
 * @return        보간된 정규화 영역
 */
QRectF interpolatedRect(const QRectF& first, const QRectF& second, double ratio) {
    const QPointF topLeft = first.topLeft() + (second.topLeft() - first.topLeft()) * ratio;
    const QPointF bottomRight = first.bottomRight() + (second.bottomRight() - first.bottomRight()) * ratio;
    return QRectF(topLeft, bottomRight);
}

/**
 * @brief          마지막 영역을 이동 방향으로 밀어 현재 위치를 예측합니다.
 * @param earlier  기준 구간이 시작하는 시각의 영역
 * @param latest   가장 최신 영역
 * @param ratio    기준 구간 길이 대비 앞서 예측할 시간의 비
 * @return         예측된 정규화 영역
 *
 * @details 이동만 반영하고 크기는 예측하지 않는다. 크기 변화는 이동보다 느린데 같은 비율로 밀면
 *          상자가 사라지거나 터지는 쪽으로 먼저 어긋난다.
 *
 *          예측 상자와 마지막 상자의 합집합을 쓰지 않는다. 합집합은 원의 중심을 두 위치의
 *          가운데로 끌어와 실제로 앞서는 양을 절반으로 깎는다. 대상이 이미 떠난 자리를 덮는 것은
 *          가림에 보탬이 되지 않고, 예측이 빗나갔을 때의 여유는 원형 마스크가 padding까지 포함한
 *          상자의 대각선 절반을 반지름으로 쓰는 데서 이미 나온다.
 */
QRectF extrapolatedRect(const QRectF& earlier, const QRectF& latest, double ratio) {
    return latest.translated((latest.center() - earlier.center()) * ratio);
}

/**
 * @brief         두 블러 영역을 (id, 대상 유형) 순서로 비교합니다.
 * @param left    왼쪽 영역
 * @param right   오른쪽 영역
 * @return        left가 앞서면 true
 */
bool blurRegionKeyLess(const BlurRegionData& left, const BlurRegionData& right) {
    if (left.id != right.id) {
        return left.id < right.id;
    }
    return static_cast<int>(left.targetType) < static_cast<int>(right.targetType);
}

/**
 * @brief          같은 (id, 대상 유형)의 영역을 이진 탐색으로 찾습니다.
 * @param regions  submitFrame()에서 이미 정렬해 둔 영역 목록
 * @param key      찾을 영역
 * @return         찾으면 해당 영역, 없으면 nullptr
 *
 * @details 목록은 **수신할 때 한 번** 정렬됩니다. 예전에는 화면 프레임마다 포인터 색인을 새로
 *          만들어 정렬했는데, 그 자리가 하필 metadata를 넣는 스레드와 같은 뮤텍스 안이라
 *          영상 스레드가 프레임마다 락을 쥔 채 힙 할당 두 번과 정렬 두 번을 했습니다.
 *          채널 넷이면 초당 수백 번이라 그대로 영상이 끊깁니다.
 */
const BlurRegionData* findSortedRegion(const QVector<BlurRegionData>& regions, const BlurRegionData& key) {
    const auto position = std::lower_bound(regions.cbegin(), regions.cend(), key, blurRegionKeyLess);
    if (position == regions.cend() || position->id != key.id || position->targetType != key.targetType) {
        return nullptr;
    }
    return &*position;
}

/** @brief 평면 좌표계로 환산한 원형 블러 영역 */
struct BlurRegionGeometry {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    double centerX = 0.0;
    double centerY = 0.0;
    double radius = 0.0;
};

/**
 * @brief             정규화 상자를 프레임 픽셀 기준 원형 영역으로 바꿉니다.
 * @param sourceBox   정규화된 블러 영역
 * @param frameWidth  프레임 가로 픽셀 수
 * @param frameHeight 프레임 세로 픽셀 수
 * @param geometry    변환된 영역
 * @return            블러를 적용할 만한 크기면 true
 */
bool blurRegionGeometry(const QRectF& sourceBox, int frameWidth, int frameHeight, const BlurProcessorConfig& config,
                        BlurRegionGeometry& geometry) {
    if (frameWidth <= 0 || frameHeight <= 0) {
        return false;
    }

    const double paddingX = sourceBox.width() * config.paddingRatio;
    const double paddingY = sourceBox.height() * config.paddingRatio;
    const QRectF paddedBox =
        sourceBox.adjusted(-paddingX, -paddingY, paddingX, paddingY).intersected(QRectF(0.0, 0.0, 1.0, 1.0));

    const int boxLeft = normalizedFloorPixel(paddedBox.left(), frameWidth);
    const int boxTop = normalizedFloorPixel(paddedBox.top(), frameHeight);
    const int boxRight = normalizedCeilPixel(paddedBox.right(), frameWidth);
    const int boxBottom = normalizedCeilPixel(paddedBox.bottom(), frameHeight);
    geometry.centerX = static_cast<double>(boxLeft + boxRight) / 2.0;
    geometry.centerY = static_cast<double>(boxTop + boxBottom) / 2.0;
    geometry.radius =
        std::hypot(static_cast<double>(boxRight - boxLeft), static_cast<double>(boxBottom - boxTop)) / 2.0;

    geometry.left = boundedFloorPixel(geometry.centerX - geometry.radius, frameWidth);
    geometry.top = boundedFloorPixel(geometry.centerY - geometry.radius, frameHeight);
    geometry.width = boundedCeilPixel(geometry.centerX + geometry.radius, frameWidth) - geometry.left;
    geometry.height = boundedCeilPixel(geometry.centerY + geometry.radius, frameHeight) - geometry.top;
    return geometry.width >= 2 && geometry.height >= 2;
}

/**
 * @brief       휘도 영역에 대응하는 절반 해상도 색차 영역을 만듭니다.
 * @param luma  휘도 평면 기준 영역
 * @return      색차 평면(가로·세로 1/2) 기준 영역
 */
BlurRegionGeometry chromaRegionGeometry(const BlurRegionGeometry& luma) {
    BlurRegionGeometry chroma;
    chroma.left = luma.left / 2;
    chroma.top = luma.top / 2;
    chroma.width = (luma.left + luma.width + 1) / 2 - chroma.left;
    chroma.height = (luma.top + luma.height + 1) / 2 - chroma.top;
    chroma.centerX = luma.centerX / 2.0;
    chroma.centerY = luma.centerY / 2.0;
    chroma.radius = luma.radius / 2.0;
    return chroma;
}

/**
 * @brief          평면 하나의 지정 원형 영역에 box blur를 적용합니다.
 * @param plane    평면 전체를 감싼 행렬(휘도 CV_8UC1, 색차 CV_8UC2)
 * @param region   평면 좌표 기준 원형 영역
 * @param radius   box blur 반경(샘플)
 *
 * @details 색차 평면은 U와 V가 번갈아 놓인 2채널로 감쌉니다. cv::blur가 채널별로 따로
 *          처리하므로 예전처럼 성분 개수를 템플릿 인자로 넘길 필요가 없습니다.
 *
 *          블러는 외접 사각형 전체에 걸고 **원 안쪽만 되돌려 씁니다.** 원 밖 모서리까지
 *          계산하게 되지만(면적으로 약 1.27배) cv::blur가 SIMD로 도는 쪽이 원 안만 스칼라로
 *          훑던 것보다 빠릅니다. 마스크가 없으면 상자 모서리가 각지게 드러납니다.
 *
 *          경계 처리가 예전과 다릅니다. 예전에는 영역 가장자리에서 창을 줄여 실제 개수로
 *          나눴고, 지금은 부모 평면의 바깥 픽셀을 그대로 읽습니다(BORDER_ISOLATED를 주지
 *          않습니다). 영역 밖은 어차피 원본 영상이라 가장자리가 더 자연스럽게 이어집니다.
 */
void blurPlaneRegion(cv::Mat& plane, const BlurRegionGeometry& region, int radius) {
    if (plane.empty() || region.width < 2 || region.height < 2 || radius < 1 || radius > maximumSupportedRadius) {
        return;
    }

    // 영역은 blurRegionGeometry가 이미 평면 안으로 잘라 두지만, 색차 평면은 홀수 크기에서
    // 반올림이 들어가므로 한 번 더 맞춘다. 여기서 벗어나면 cv::Mat이 예외를 던진다
    const cv::Rect bounds(0, 0, plane.cols, plane.rows);
    const cv::Rect box = cv::Rect(region.left, region.top, region.width, region.height) & bounds;
    if (box.width < 2 || box.height < 2) {
        return;
    }

    // 프레임마다 다시 할당하지 않는다. 채널마다 영상 스레드가 하나씩이라 thread_local이
    // 곧 채널별 버퍼가 된다(예전 scratch_ 멤버와 같은 수명이다)
    thread_local cv::Mat blurred;
    thread_local cv::Mat mask;

    cv::Mat roi = plane(box);
    const int kernelSize = 2 * radius + 1;
    cv::blur(roi, blurred, cv::Size(kernelSize, kernelSize));

    // 마스크는 매번 지운다. 앞 영역이 더 컸으면 그 자국이 남아 원 밖까지 덮어쓴다
    mask.create(box.height, box.width, CV_8UC1);
    mask.setTo(cv::Scalar(0));
    const cv::Point center(static_cast<int>(region.centerX) - box.x, static_cast<int>(region.centerY) - box.y);
    cv::circle(mask, center, static_cast<int>(region.radius), cv::Scalar(255), cv::FILLED);

    blurred.copyTo(roi, mask);
}

/**
 * @brief           NV12 프레임의 지정 영역을 휘도·색차 평면 모두에 블러 처리합니다.
 * @param frame     수정할 영상 프레임
 * @param sourceBox 정규화된 블러 영역
 * @param scratch   재사용할 중간 버퍼
 *
 * @details 디코더가 내는 NV12를 그대로 처리해 BGRA 변환을 없앤다. 같은 영역이라도 다루는
 *          바이트가 4바이트/픽셀에서 1.5바이트/픽셀로 줄어 연산량도 함께 줄어든다.
 */
void applyNv12Blur(GstVideoFrame& frame, const QRectF& sourceBox, const BlurProcessorConfig& config) {
    BlurRegionGeometry luma;
    if (!blurRegionGeometry(sourceBox, GST_VIDEO_FRAME_WIDTH(&frame), GST_VIDEO_FRAME_HEIGHT(&frame), config, luma)) {
        return;
    }

    const int lumaRadius = std::clamp(std::min(luma.width, luma.height) / config.radiusDivisor, config.minimumRadius,
                                      config.maximumRadius);
    // NV12 평면을 복사 없이 감싼다. 색공간 변환이 없으므로 프레임당 추가 비용은 블러 자체뿐이다
    cv::Mat lumaPlane(GST_VIDEO_FRAME_HEIGHT(&frame), GST_VIDEO_FRAME_WIDTH(&frame), CV_8UC1,
                      GST_VIDEO_FRAME_PLANE_DATA(&frame, 0),
                      static_cast<size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0)));
    blurPlaneRegion(lumaPlane, luma, lumaRadius);

    // 색차는 U와 V가 번갈아 놓인 절반 해상도 평면이라 2채널로 감싸고 반경도 절반으로 본다
    cv::Mat chromaPlane(GST_VIDEO_FRAME_HEIGHT(&frame) / 2, GST_VIDEO_FRAME_WIDTH(&frame) / 2, CV_8UC2,
                        GST_VIDEO_FRAME_PLANE_DATA(&frame, 1),
                        static_cast<size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1)));
    blurPlaneRegion(chromaPlane, chromaRegionGeometry(luma), std::max(1, lumaRadius / 2));
}
}  // namespace

/**
 * @brief        블러 프레임 처리기를 생성합니다.
 * @param config JSON 검증을 통과한 블러 동기화 및 렌더링 설정
 */
BlurProcessor::BlurProcessor(BlurProcessorConfig config) : config_(std::move(config)) { metadataClock_.start(); }

/**
 * @brief                     블러를 적용할 객체 유형을 설정합니다.
 * @param faceEnabled         얼굴 블러 활성화 여부
 * @param licensePlateEnabled 차량 번호판 블러 활성화 여부
 */
void BlurProcessor::setTargetsEnabled(bool faceEnabled, bool licensePlateEnabled) {
    faceEnabled_.store(faceEnabled, std::memory_order_release);
    licensePlateEnabled_.store(licensePlateEnabled, std::memory_order_release);
}

/**
 * @brief       MQTT에서 수신한 채널별 블러 좌표를 시간순으로 저장합니다.
 * @param frame 블러 메타데이터 프레임
 */
void BlurProcessor::submitFrame(BlurFrameData frame) {
    if (frame.channelIndex < 0 || frame.sourceTimestamp <= 0) {
        return;
    }

    // 영역을 (id, 대상 유형)으로 여기서 정렬해 둡니다. 보간이 이전·다음 프레임에서 같은 id를
    // 찾을 때 이진 탐색만 하면 되고, 화면 프레임마다 정렬할 일이 없어집니다.
    // 락을 잡기 전에 하므로 이 정렬은 락 구간을 늘리지 않습니다.
    std::stable_sort(frame.regions.begin(), frame.regions.end(), blurRegionKeyLess);

    const qint64 arrivalTimeMsec = qMax<qint64>(1, metadataClock_.elapsed());
    QMutexLocker locker(&mutex_);

    // lastMetadataArrivalMsec_는 마지막으로 "승인한" metadata 시각입니다.
    // reject된 stale metadata가 이 값을 갱신하면 source restart 감지가 영원히 미뤄질 수 있습니다.
    const qint64 lastAcceptedArrivalMsec = lastMetadataArrivalMsec_;
    const bool acceptedFrameGapExpired =
        lastAcceptedArrivalMsec > 0 && arrivalTimeMsec - lastAcceptedArrivalMsec >= config_.sourceRestartGapMsec;
    // upstream/dispatcher가 재시작했거나 timestamp 기준점이 오염된 뒤 정상 값으로 돌아온 경우
    // 일정 시간 동안 정상 metadata가 승인되지 않으면 history 전체를 재동기화합니다.
    if (acceptedFrameGapExpired) {
        history_.clear();
        latestSourceTimestamp_ = 0;
    }

    // history 범위보다 지나치게 오래된 metadata는 사용하지 않습니다.
    // 중요: 여기서 return하더라도 lastMetadataArrivalMsec_는 갱신하지 않습니다.
    if (latestSourceTimestamp_ > 0 && frame.sourceTimestamp < latestSourceTimestamp_ - config_.historyMsec) {
        return;
    }

    // 승인된 metadata만 복구 감시 시각과 최신 timestamp를 전진시킵니다.
    lastMetadataArrivalMsec_ = arrivalTimeMsec;
    latestSourceTimestamp_ = std::max(latestSourceTimestamp_, frame.sourceTimestamp);
    channelIndex_.store(frame.channelIndex, std::memory_order_relaxed);

    if (history_.isEmpty() || frame.sourceTimestamp > history_.constLast().sourceTimestamp) {
        history_.append(std::move(frame));
    } else if (frame.sourceTimestamp == history_.constLast().sourceTimestamp) {
        history_.last() = std::move(frame);
    } else {
        const auto position = std::lower_bound(
            history_.begin(), history_.end(), frame.sourceTimestamp,
            [](const BlurFrameData& stored, qint64 timestamp) { return stored.sourceTimestamp < timestamp; });

        if (position != history_.end() && position->sourceTimestamp == frame.sourceTimestamp) {
            *position = std::move(frame);
        } else {
            history_.insert(position, std::move(frame));
        }
    }

    const qint64 newestTimestamp = history_.constLast().sourceTimestamp;
    qsizetype removeCount = 0;
    while (removeCount < history_.size() &&
           (history_[removeCount].sourceTimestamp < newestTimestamp - config_.historyMsec ||
            history_.size() - removeCount > config_.maximumHistorySize)) {
        ++removeCount;
    }
    if (removeCount > 0) {
        history_.remove(0, removeCount);
    }
}

/**
 * @brief        RTP PTS와 RTCP sender 시각의 대응 관계를 영상 clock mapper에 전달합니다.
 * @param buffer rtspsrc 내부 jitterbuffer를 통과한 영상 buffer
 */
void BlurProcessor::observeVideoBuffer(const GstBuffer* buffer) { utcClockMapper_.observe(buffer); }

/**
 * @brief 저장된 블러 좌표와 동기화 상태를 초기화합니다.
 */
void BlurProcessor::clear() {
    QMutexLocker locker(&mutex_);
    history_.clear();
    latestSourceTimestamp_ = 0;
    lastMetadataArrivalMsec_ = 0;
    utcClockMapper_.reset();
}

/**
 * @brief       GStreamer가 제공한 쓰기 가능한 영상 프레임에 현재 시각과 가장 가까운 블러 좌표를 적용합니다.
 * @param frame NV12 영상 프레임
 */
void BlurProcessor::apply(GstVideoFrame& frame) {
    // 두 대상이 모두 꺼져 있으면 어차피 그릴 영역이 없다. 시각 변환과 이력 조회(락)까지
    // 가기 전에 끊는다
    if (!faceEnabled_.load(std::memory_order_acquire) && !licensePlateEnabled_.load(std::memory_order_acquire)) {
        return;
    }

    // 블러 연산뿐 아니라 이력 조회까지 함께 잰다. regionsFor는 metadata를 넣는 dispatcher 스레드와
    // 같은 뮤텍스를 쓰므로, 영상 스레드가 프레임당 막힐 수 있는 유일한 자리가 거기다.
    // 연산만 재면 락 대기가 통째로 빠져 "블러는 빠른데 왜 끊기지"로 잘못 읽힌다
    QElapsedTimer processingTimer;
    if (config_.debugLogIntervalMsec > 0) {
        processingTimer.start();
    }

    const std::optional<VideoUtcTimestamp> frameTimestamp = utcClockMapper_.timestampFor(frame.buffer);
    if (!frameTimestamp.has_value()) {
        return;
    }

    const qint64 fallbackOffset = frameTimestamp->senderClock ? 0 : config_.syncOffsetMsec;
    const qint64 targetTimestamp = frameTimestamp->utcMsec - fallbackOffset;
    qint64 metadataLagMsec = 0;
    const QVector<QRectF> regions = regionsFor(targetTimestamp, metadataLagMsec);
    if (regions.isEmpty()) {
        return;
    }

    if (GST_VIDEO_FRAME_FORMAT(&frame) != GST_VIDEO_FORMAT_NV12) {
        return;
    }

    for (const QRectF& region : regions) {
        applyNv12Blur(frame, region, config_);
    }

    if (config_.debugLogIntervalMsec <= 0) {
        return;
    }

    // 진단 로그가 꺼져 있으면 시계도 읽지 않는다. 매 프레임·채널마다 부르던 호출이다
    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
    qint64 lastLogMsec = lastApplyLogMsec_.load(std::memory_order_relaxed);
    if (nowMsec - lastLogMsec >= config_.debugLogIntervalMsec &&
        lastApplyLogMsec_.compare_exchange_strong(lastLogMsec, nowMsec, std::memory_order_relaxed)) {
        qInfo().noquote() << QStringLiteral(
                                 "[BLUR APPLY] channel=%1 regions=%2 frame=%3x%4 targetTs=%5 clock=%6 "
                                 "metadataLagMs=%7 processingUs=%8")
                                 .arg(channelIndex_.load(std::memory_order_relaxed))
                                 .arg(regions.size())
                                 .arg(GST_VIDEO_FRAME_WIDTH(&frame))
                                 .arg(GST_VIDEO_FRAME_HEIGHT(&frame))
                                 .arg(targetTimestamp)
                                 .arg(frameTimestamp->senderClock ? QStringLiteral("rtcp")
                                                                  : QStringLiteral("pts-anchor"))
                                 .arg(metadataLagMsec)
                                 .arg(processingTimer.nsecsElapsed() / 1000);
    }
}

/**
 * @brief                   지정 시각과 가장 가까운 블러 영역을 조회합니다.
 * @param sourceTimestamp   영상에 대응시킬 원본 시각
 * @param metadataLagMsec   가장 최신 metadata가 이 프레임보다 얼마나 뒤처져 있는지(진단용).
 *                          양수면 그만큼 예측으로 메워야 하는 구간이다
 * @return                  정규화된 블러 영역 목록
 */
QVector<QRectF> BlurProcessor::regionsFor(qint64 sourceTimestamp, qint64& metadataLagMsec) const {
    QMutexLocker locker(&mutex_);
    if (history_.isEmpty()) {
        return {};
    }

    metadataLagMsec = sourceTimestamp - history_.constLast().sourceTimestamp;

    const auto after = std::lower_bound(
        history_.cbegin(), history_.cend(), sourceTimestamp,
        [](const BlurFrameData& frame, qint64 timestamp) { return frame.sourceTimestamp < timestamp; });

    const BlurFrameData* nextFrame = after == history_.cend() ? nullptr : &*after;
    const BlurFrameData* previousFrame = after == history_.cbegin() ? nullptr : &*std::prev(after);
    const BlurFrameData* nearest = nextFrame;
    if (after != history_.cbegin()) {
        const BlurFrameData& before = *previousFrame;
        if (nearest == nullptr ||
            sourceTimestamp - before.sourceTimestamp <= nearest->sourceTimestamp - sourceTimestamp) {
            nearest = &before;
        }
    }
    const bool nearestMatches =
        nearest && std::llabs(nearest->sourceTimestamp - sourceTimestamp) <= config_.matchToleranceMsec;
    const bool canHoldPrevious = previousFrame && sourceTimestamp >= previousFrame->sourceTimestamp &&
                                 sourceTimestamp - previousFrame->sourceTimestamp <= config_.holdLastMetadataMsec;
    const BlurFrameData* selectedFrame = nearestMatches ? nearest : (canHoldPrevious ? previousFrame : nullptr);
    if (!selectedFrame) {
        return {};
    }

    double interpolationRatio = 0.0;
    const bool canInterpolate =
        nearestMatches && previousFrame && nextFrame && nextFrame->sourceTimestamp > previousFrame->sourceTimestamp &&
        std::llabs(sourceTimestamp - previousFrame->sourceTimestamp) <= config_.matchToleranceMsec &&
        std::llabs(nextFrame->sourceTimestamp - sourceTimestamp) <= config_.matchToleranceMsec;
    if (canInterpolate) {
        interpolationRatio =
            std::clamp(static_cast<double>(sourceTimestamp - previousFrame->sourceTimestamp) /
                           static_cast<double>(nextFrame->sourceTimestamp - previousFrame->sourceTimestamp),
                       0.0, 1.0);
    }

    // 다음 metadata가 아직 도착하지 않아 마지막 프레임을 그대로 쓰는 구간입니다. 검출 서버의
    // 처리·전송 지연이 영상 지연보다 길면 정상 동작 중에도 여기가 상시 경로가 되고, 그 동안
    // 상자가 멈춰 있어 움직이는 대상의 앞쪽이 그대로 드러납니다. 최근 구간에서 잰 이동 속도로
    // 현재 위치를 예측해 덮습니다.
    //
    // 영상 지연을 늘려 metadata를 기다리는 방법이 정렬로는 정확하지만, sink 지연은 영상 끊김과
    // 직결됩니다. 예측은 이미 잡고 있는 뮤텍스 안에서 상자 산술만 하므로 영상 경로에 비용이 없습니다.
    const BlurFrameData* velocityFrame = nullptr;
    double extrapolationRatio = 0.0;
    if (nextFrame == nullptr && selectedFrame == previousFrame && config_.maximumExtrapolationMsec > 0 &&
        history_.size() >= 2) {
        // 속도는 예측 한계와 같은 길이의 구간에서 잽니다. 바로 직전 프레임 하나만 쓰면 검출 상자의
        // 떨림이 그대로 속도가 되어 예측이 튀고, 구간이 길수록 그 떨림은 구간 길이로 나뉩니다.
        // 그 구간 안에 이전 프레임이 없을 만큼 metadata가 드물면 속도를 믿지 않고 예측을 건너뜁니다.
        // 예측할 수 있는 시간과 속도를 잴 수 있는 시간을 같은 값 하나로 유지합니다.
        const qint64 baselineStartMsec = previousFrame->sourceTimestamp - config_.maximumExtrapolationMsec;
        const auto oldest = std::lower_bound(
            history_.cbegin(), history_.cend(), baselineStartMsec,
            [](const BlurFrameData& frame, qint64 timestamp) { return frame.sourceTimestamp < timestamp; });
        const qint64 baselineMsec =
            oldest == history_.cend() ? 0 : previousFrame->sourceTimestamp - oldest->sourceTimestamp;
        const qint64 aheadMsec =
            std::min(sourceTimestamp - previousFrame->sourceTimestamp, config_.maximumExtrapolationMsec);
        if (baselineMsec > 0 && aheadMsec > 0) {
            velocityFrame = &*oldest;
            extrapolationRatio = static_cast<double>(aheadMsec) / static_cast<double>(baselineMsec);
        }
    }

    QVector<QRectF> regions;
    regions.reserve(selectedFrame->regions.size());
    for (const BlurRegionData& region : selectedFrame->regions) {
        const bool enabled = region.targetType == BlurTargetType::Face
                                 ? faceEnabled_.load(std::memory_order_acquire)
                                 : licensePlateEnabled_.load(std::memory_order_acquire);
        if (!enabled) {
            continue;
        }

        QRectF normalizedBox = region.normalizedBox;
        if (canInterpolate) {
            const BlurRegionData* previousRegion = findSortedRegion(previousFrame->regions, region);
            const BlurRegionData* nextRegion = findSortedRegion(nextFrame->regions, region);
            if (previousRegion && nextRegion) {
                normalizedBox =
                    interpolatedRect(previousRegion->normalizedBox, nextRegion->normalizedBox, interpolationRatio);
            }
        } else if (velocityFrame) {
            if (const BlurRegionData* earlierRegion = findSortedRegion(velocityFrame->regions, region)) {
                normalizedBox = extrapolatedRect(earlierRegion->normalizedBox, normalizedBox, extrapolationRatio);
            }
        }
        regions.append(normalizedBox);
    }
    return regions;
}
