#include "video/BlurProcessor.h"

#include <gst/video/video-frame.h>

#include <QDateTime>
#include <QDebug>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace {
/**
 * @brief               한 열에서 원 안쪽으로 허용되는 세로 거리 제곱을 구합니다.
 * @param x             픽셀 X 좌표
 * @param centerX       원의 중심 X 좌표
 * @param centerY       원의 중심 Y 좌표
 * @param radius        원의 반지름
 * @return              세로 거리 제곱의 상한. 음수면 그 열은 원 밖이다
 *
 * @details 열이 바뀔 때만 달라지는 항이라 안쪽 세로 루프 밖으로 뺀다. 픽셀마다 남는 계산은
 *          세로 차이 제곱 하나와 비교 하나뿐이다.
 */
double verticalDistanceLimit(int x, double centerX, double radius) {
    const double deltaX = static_cast<double>(x) + 0.5 - centerX;
    return radius * radius - deltaX * deltaX;
}

/**
 * @brief                 픽셀이 원형 블러 영역의 세로 범위 안인지 확인합니다.
 * @param y               픽셀 Y 좌표
 * @param centerY         원의 중심 Y 좌표
 * @param distanceLimit   verticalDistanceLimit()이 구한 상한
 * @return                원 안쪽이면 true
 */
bool isInsideVerticalRange(int y, double centerY, double distanceLimit) {
    const double deltaY = static_cast<double>(y) + 0.5 - centerY;
    return deltaY * deltaY <= distanceLimit;
}

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
 * @brief                 평면 하나의 지정 영역에 2-pass box blur를 적용합니다.
 * @param pixels          평면 시작 주소
 * @param stride          평면 한 줄의 바이트 수
 * @param pixelStride     한 픽셀(샘플)이 차지하는 바이트 수
 * @param componentCount  픽셀 안에서 블러할 성분 개수
 * @param region          평면 좌표 기준 원형 영역
 * @param radius          box blur 반경(샘플)
 * @param scratch         재사용할 중간 버퍼
 *
 * @details 가로 패스는 슬라이딩 합이라 반경과 무관하게 영역 면적에 선형이다. NV12는 휘도
 *          평면(성분 1개, 간격 1바이트)과 색차 평면(U,V 2개, 간격 2바이트)을 각각 호출한다.
 */
void blurPlaneRegion(guint8* pixels, int stride, int pixelStride, int componentCount, const BlurRegionGeometry& region,
                     int radius, std::vector<guint8>& scratch) {
    if (!pixels || region.width < 2 || region.height < 2 || radius < 1) {
        return;
    }

    const size_t scratchSize = static_cast<size_t>(region.width) * region.height * componentCount;
    if (scratch.size() < scratchSize) {
        scratch.resize(scratchSize);
    }

    for (int localY = 0; localY < region.height; ++localY) {
        const guint8* sourceRow = pixels + (region.top + localY) * stride + region.left * pixelStride;
        for (int component = 0; component < componentCount; ++component) {
            quint64 sum = 0;
            int windowEnd = std::min(radius, region.width - 1);
            for (int x = 0; x <= windowEnd; ++x) {
                sum += sourceRow[x * pixelStride + component];
            }

            for (int x = 0; x < region.width; ++x) {
                const int windowStart = std::max(0, x - radius);
                windowEnd = std::min(region.width - 1, x + radius);
                const int count = windowEnd - windowStart + 1;
                scratch[(static_cast<size_t>(localY) * region.width + x) * componentCount + component] =
                    static_cast<guint8>(sum / static_cast<quint64>(count));

                const int removeX = x - radius;
                const int addX = x + radius + 1;
                if (removeX >= 0) {
                    sum -= sourceRow[removeX * pixelStride + component];
                }
                if (addX < region.width) {
                    sum += sourceRow[addX * pixelStride + component];
                }
            }
        }
    }

    for (int localX = 0; localX < region.width; ++localX) {
        const double distanceLimit = verticalDistanceLimit(region.left + localX, region.centerX, region.radius);
        if (distanceLimit < 0.0) {
            // 이 열은 통째로 원 밖이라 세로 합을 굴릴 필요가 없다
            continue;
        }

        for (int component = 0; component < componentCount; ++component) {
            quint64 sum = 0;
            int windowEnd = std::min(radius, region.height - 1);
            for (int y = 0; y <= windowEnd; ++y) {
                sum += scratch[(static_cast<size_t>(y) * region.width + localX) * componentCount + component];
            }

            for (int localY = 0; localY < region.height; ++localY) {
                const int windowStart = std::max(0, localY - radius);
                windowEnd = std::min(region.height - 1, localY + radius);
                const int count = windowEnd - windowStart + 1;
                if (isInsideVerticalRange(region.top + localY, region.centerY, distanceLimit)) {
                    guint8* targetPixel =
                        pixels + (region.top + localY) * stride + (region.left + localX) * pixelStride;
                    targetPixel[component] = static_cast<guint8>(sum / static_cast<quint64>(count));
                }

                const int removeY = localY - radius;
                const int addY = localY + radius + 1;
                if (removeY >= 0) {
                    sum -= scratch[(static_cast<size_t>(removeY) * region.width + localX) * componentCount + component];
                }
                if (addY < region.height) {
                    sum += scratch[(static_cast<size_t>(addY) * region.width + localX) * componentCount + component];
                }
            }
        }
    }
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
void applyNv12Blur(GstVideoFrame& frame, const QRectF& sourceBox, std::vector<guint8>& scratch,
                   const BlurProcessorConfig& config) {
    BlurRegionGeometry luma;
    if (!blurRegionGeometry(sourceBox, GST_VIDEO_FRAME_WIDTH(&frame), GST_VIDEO_FRAME_HEIGHT(&frame), config, luma)) {
        return;
    }

    const int lumaRadius = std::clamp(std::min(luma.width, luma.height) / config.radiusDivisor, config.minimumRadius,
                                      config.maximumRadius);
    blurPlaneRegion(static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), 1, 1, luma, lumaRadius, scratch);

    // 색차는 U와 V가 번갈아 놓인 절반 해상도 평면이라 반경도 절반으로 본다
    blurPlaneRegion(static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1)),
                    GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1), 2, 2, chromaRegionGeometry(luma),
                    std::max(1, lumaRadius / 2), scratch);
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

    const std::optional<VideoUtcTimestamp> frameTimestamp = utcClockMapper_.timestampFor(frame.buffer);
    if (!frameTimestamp.has_value()) {
        return;
    }

    const qint64 fallbackOffset = frameTimestamp->senderClock ? 0 : config_.syncOffsetMsec;
    const qint64 targetTimestamp = frameTimestamp->utcMsec - fallbackOffset;
    const QVector<QRectF> regions = regionsFor(targetTimestamp);
    if (regions.isEmpty()) {
        return;
    }

    if (GST_VIDEO_FRAME_FORMAT(&frame) != GST_VIDEO_FORMAT_NV12) {
        return;
    }

    for (const QRectF& region : regions) {
        applyNv12Blur(frame, region, scratch_, config_);
    }

    if (config_.debugLogIntervalMsec <= 0) {
        return;
    }

    // 진단 로그가 꺼져 있으면 시계도 읽지 않는다. 매 프레임·채널마다 부르던 호출이다
    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
    qint64 lastLogMsec = lastApplyLogMsec_.load(std::memory_order_relaxed);
    if (nowMsec - lastLogMsec >= config_.debugLogIntervalMsec &&
        lastApplyLogMsec_.compare_exchange_strong(lastLogMsec, nowMsec, std::memory_order_relaxed)) {
        qInfo().noquote() << QStringLiteral("[BLUR APPLY] channel=%1 regions=%2 frame=%3x%4 targetTs=%5 clock=%6")
                                 .arg(channelIndex_.load(std::memory_order_relaxed))
                                 .arg(regions.size())
                                 .arg(GST_VIDEO_FRAME_WIDTH(&frame))
                                 .arg(GST_VIDEO_FRAME_HEIGHT(&frame))
                                 .arg(targetTimestamp)
                                 .arg(frameTimestamp->senderClock ? QStringLiteral("rtcp")
                                                                  : QStringLiteral("pts-anchor"));
    }
}

/**
 * @brief                  지정 시각과 가장 가까운 블러 영역을 조회합니다.
 * @param sourceTimestamp  영상에 대응시킬 원본 시각
 * @return                 정규화된 블러 영역 목록
 */
QVector<QRectF> BlurProcessor::regionsFor(qint64 sourceTimestamp) const {
    QMutexLocker locker(&mutex_);
    if (history_.isEmpty()) {
        return {};
    }

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
            const auto previousRegion =
                std::find_if(previousFrame->regions.cbegin(), previousFrame->regions.cend(),
                             [&region](const BlurRegionData& candidate) {
                                 return candidate.id == region.id && candidate.targetType == region.targetType;
                             });
            const auto nextRegion = std::find_if(
                nextFrame->regions.cbegin(), nextFrame->regions.cend(), [&region](const BlurRegionData& candidate) {
                    return candidate.id == region.id && candidate.targetType == region.targetType;
                });
            if (previousRegion != previousFrame->regions.cend() && nextRegion != nextFrame->regions.cend()) {
                normalizedBox =
                    interpolatedRect(previousRegion->normalizedBox, nextRegion->normalizedBox, interpolationRatio);
            }
        }
        regions.append(normalizedBox);
    }
    return regions;
}
