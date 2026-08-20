#include <gst/gst.h>
#include <gst/video/video-frame.h>
#include <gst/video/video-info.h>

#include <QDateTime>
#include <cstdio>
#include <vector>

#include "video/BlurProcessor.h"

namespace {
constexpr int frameWidth = 320;
constexpr int frameHeight = 240;
constexpr guint8 backgroundLuma = 40;
constexpr guint8 backgroundChroma = 128;
constexpr guint8 patternLuma = 220;
constexpr guint8 patternChroma = 30;

int failureCount = 0;

void check(bool condition, const char* description) {
    if (condition) {
        return;
    }

    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failureCount;
}

BlurProcessorConfig makeConfig() {
    BlurProcessorConfig config;
    config.syncOffsetMsec = 0;
    config.historyMsec = 10000;
    config.matchToleranceMsec = 250;
    config.holdLastMetadataMsec = 1000;
    config.maximumExtrapolationMsec = 0;
    config.maximumHistorySize = 300;
    config.sourceRestartGapMsec = 5000;
    config.paddingRatio = 0.18;
    config.radiusDivisor = 3;
    config.minimumRadius = 4;
    config.maximumRadius = 28;
    config.debugLogIntervalMsec = 0;
    return config;
}

/**
 * @brief   가운데에 밝은 사각형이 있는 NV12 buffer를 만듭니다.
 * @return  PTS가 0인 쓰기 가능한 buffer
 */
GstBuffer* createPatternBuffer(const GstVideoInfo& info) {
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, GST_VIDEO_INFO_SIZE(&info), nullptr);
    GstVideoFrame frame;
    if (!buffer || !gst_video_frame_map(&frame, const_cast<GstVideoInfo*>(&info), buffer, GST_MAP_WRITE)) {
        return buffer;
    }

    auto* luma = static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const int lumaStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    for (int y = 0; y < frameHeight; ++y) {
        for (int x = 0; x < frameWidth; ++x) {
            const bool inPattern = x >= frameWidth / 4 && x < frameWidth * 3 / 4 && y >= frameHeight / 4 &&
                                   y < frameHeight * 3 / 4 && ((x / 4) % 2 == 0);
            luma[y * lumaStride + x] = inPattern ? patternLuma : backgroundLuma;
        }
    }

    auto* chroma = static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
    const int chromaStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
    for (int y = 0; y < frameHeight / 2; ++y) {
        for (int x = 0; x < frameWidth / 2; ++x) {
            const bool inPattern = x >= frameWidth / 8 && x < frameWidth * 3 / 8 && ((x / 2) % 2 == 0);
            chroma[y * chromaStride + x * 2] = inPattern ? patternChroma : backgroundChroma;
            chroma[y * chromaStride + x * 2 + 1] = inPattern ? patternChroma : backgroundChroma;
        }
    }

    gst_video_frame_unmap(&frame);
    GST_BUFFER_PTS(buffer) = 0;
    return buffer;
}

guint8 lumaAt(const GstVideoFrame& frame, int x, int y) {
    const auto* plane = static_cast<const guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    return plane[y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0) + x];
}

guint8 chromaAt(const GstVideoFrame& frame, int x, int y, int component) {
    const auto* plane = static_cast<const guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
    return plane[y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1) + x * 2 + component];
}

/**
 * @brief 화면 가운데 상자에만 블러가 적용되고 두 평면 모두 부드러워지는지 검사합니다.
 */
void checkNv12RegionBlur() {
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, frameWidth, frameHeight);

    BlurProcessor processor(makeConfig());
    GstBuffer* buffer = createPatternBuffer(info);
    check(buffer != nullptr, "NV12 buffer must be allocated");
    if (!buffer) {
        return;
    }

    // PTS 0을 기준점으로 삼고 같은 시각의 메타데이터를 넣는다
    processor.observeVideoBuffer(buffer);
    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();

    BlurFrameData metadata;
    metadata.channelIndex = 0;
    metadata.sourceTimestamp = nowMsec;
    BlurRegionData region;
    region.id = 1;
    region.targetType = BlurTargetType::Face;
    region.normalizedBox = QRectF(0.375, 0.375, 0.25, 0.25);
    metadata.regions.append(region);
    processor.submitFrame(metadata);

    GstVideoFrame frame;
    check(gst_video_frame_map(&frame, &info, buffer, GST_MAP_READWRITE) == TRUE, "frame must map read-write");

    const guint8 outsideLumaBefore = lumaAt(frame, 8, 8);
    const guint8 outsideChromaBefore = chromaAt(frame, 4, 4, 0);
    processor.apply(frame);

    // 중심은 밝은 줄과 어두운 줄이 섞여 배경값과 뚜렷이 달라야 한다
    const guint8 centerLuma = lumaAt(frame, frameWidth / 2, frameHeight / 2);
    check(centerLuma > backgroundLuma + 20 && centerLuma < patternLuma - 20,
          "blurred luma must land between background and pattern");

    // 이웃 픽셀 차이가 줄어드는 것이 블러의 정의다
    int maximumNeighbourStep = 0;
    for (int x = frameWidth / 2 - 12; x < frameWidth / 2 + 12; ++x) {
        const int step = std::abs(static_cast<int>(lumaAt(frame, x, frameHeight / 2)) -
                                  static_cast<int>(lumaAt(frame, x + 1, frameHeight / 2)));
        maximumNeighbourStep = std::max(maximumNeighbourStep, step);
    }
    check(maximumNeighbourStep < patternLuma - backgroundLuma, "neighbouring luma steps must shrink after blurring");

    const guint8 centerChroma = chromaAt(frame, frameWidth / 4, frameHeight / 4, 0);
    check(centerChroma != backgroundChroma && centerChroma != patternChroma, "chroma plane must be blurred too");
    check(chromaAt(frame, frameWidth / 4, frameHeight / 4, 1) != backgroundChroma,
          "both interleaved chroma components must be blurred");

    // 외접 사각형 안쪽이지만 원 밖인 모서리. 원 판정이 뒤집히면 여기가 먼저 깨진다
    check(lumaAt(frame, 93, 53) == backgroundLuma, "square corner outside the circle must stay untouched");

    // 영역 밖은 손대지 않는다
    check(lumaAt(frame, 8, 8) == outsideLumaBefore, "luma outside the region must stay untouched");
    check(chromaAt(frame, 4, 4, 0) == outsideChromaBefore, "chroma outside the region must stay untouched");
    check(lumaAt(frame, frameWidth - 1, frameHeight - 1) == backgroundLuma, "frame corner must stay untouched");

    gst_video_frame_unmap(&frame);
    gst_buffer_unref(buffer);
}

/**
 * @brief 중간 버퍼를 재사용해도 같은 입력이 같은 결과를 내는지 검사합니다.
 *
 * @details 가로 패스는 세로 패스가 읽어 갈 칸만 채우고, 중간 버퍼는 프레임 사이에 재사용된다.
 *          채우는 범위가 한 칸이라도 모자라면 직전 프레임이 남긴 값을 읽는데, 픽셀 값의 범위나
 *          이웃 차이만 보는 검사로는 그게 드러나지 않는다. 크기가 다른 영역을 한 번 블러해
 *          중간 버퍼를 다르게 더럽힌 뒤 같은 입력을 다시 블러해 바이트 단위로 비교한다.
 */
void checkScratchReuseIsStable() {
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, frameWidth, frameHeight);

    BlurProcessor processor(makeConfig());
    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();

    const auto blurOnce = [&](const QRectF& normalizedBox) {
        std::vector<guint8> result;
        GstBuffer* buffer = createPatternBuffer(info);
        if (!buffer) {
            return result;
        }

        processor.observeVideoBuffer(buffer);

        BlurFrameData metadata;
        metadata.channelIndex = 0;
        metadata.sourceTimestamp = nowMsec;
        BlurRegionData region;
        region.id = 1;
        region.targetType = BlurTargetType::Face;
        region.normalizedBox = normalizedBox;
        metadata.regions.append(region);
        processor.submitFrame(metadata);

        GstVideoFrame frame;
        if (gst_video_frame_map(&frame, &info, buffer, GST_MAP_READWRITE) == TRUE) {
            processor.apply(frame);

            const auto* luma = static_cast<const guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
            const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
            result.reserve(static_cast<size_t>(frameWidth) * frameHeight);
            for (int y = 0; y < frameHeight; ++y) {
                result.insert(result.end(), luma + y * stride, luma + y * stride + frameWidth);
            }
            gst_video_frame_unmap(&frame);
        }

        gst_buffer_unref(buffer);
        return result;
    };

    const QRectF probeBox(0.375, 0.375, 0.25, 0.25);
    const std::vector<guint8> first = blurOnce(probeBox);
    // 중간 버퍼를 다른 모양으로 덮어 둔다
    blurOnce(QRectF(0.05, 0.05, 0.5, 0.5));
    const std::vector<guint8> second = blurOnce(probeBox);

    check(!first.empty(), "probe frame must be blurred");
    check(first == second, "reusing the scratch buffer must not change the result");
}

/**
 * @brief 블러 대상이 모두 꺼져 있으면 프레임을 건드리지 않는지 검사합니다.
 */
void checkDisabledTargetsLeaveFrame() {
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, frameWidth, frameHeight);

    BlurProcessor processor(makeConfig());
    processor.setTargetsEnabled(false, false);
    GstBuffer* buffer = createPatternBuffer(info);
    if (!buffer) {
        return;
    }

    processor.observeVideoBuffer(buffer);
    BlurFrameData metadata;
    metadata.channelIndex = 0;
    metadata.sourceTimestamp = QDateTime::currentMSecsSinceEpoch();
    BlurRegionData region;
    region.id = 1;
    region.targetType = BlurTargetType::Face;
    region.normalizedBox = QRectF(0.375, 0.375, 0.25, 0.25);
    metadata.regions.append(region);
    processor.submitFrame(metadata);

    GstVideoFrame frame;
    gst_video_frame_map(&frame, &info, buffer, GST_MAP_READWRITE);
    const guint8 centerBefore = lumaAt(frame, frameWidth / 2, frameHeight / 2);
    processor.apply(frame);
    check(lumaAt(frame, frameWidth / 2, frameHeight / 2) == centerBefore,
          "disabled blur targets must leave the frame unchanged");
    gst_video_frame_unmap(&frame);
    gst_buffer_unref(buffer);
}

/**
 * @brief 다음 metadata가 아직 없을 때 최근 이동 속도로 예측한 위치를 덮는지 검사합니다.
 *
 * @details 검출 서버가 영상보다 늦으면 조회 시각이 항상 마지막 metadata보다 앞서고, 예측이 없으면
 *          마스크가 멈춰 움직이는 대상의 진행 방향이 드러난다. 같은 metadata를 넣고 예측만 껐다 켜서
 *          가운뎃줄에서 실제로 바뀐 구간을 비교한다. 구간의 중심이 이동량만큼 통째로 앞으로 가야 한다.
 *          예측 상자와 마지막 상자의 합집합을 덮으면 중심이 두 위치의 가운데로 끌려와 이동량의 절반만
 *          앞서므로, 그 방식으로 되돌아가면 이 검사가 걸린다.
 */
void checkExtrapolationCoversMovingObject() {
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, frameWidth, frameHeight);

    constexpr int probeRow = 120;
    // 100 ms 동안 정규화 0.08(=25.6 px)만큼 오른쪽으로 이동한다
    const QRectF earlierBox(0.20, 0.40, 0.15, 0.20);
    const QRectF latestBox(0.28, 0.40, 0.15, 0.20);
    constexpr double expectedShiftPixels = 0.08 * frameWidth;

    // 가운뎃줄에서 블러가 실제로 바꾼 구간. 못 찾으면 폭이 음수인 구간을 돌려준다
    const auto blurredSpan = [&](qint64 maximumExtrapolationMsec, int& first, int& last) {
        first = frameWidth;
        last = -1;

        BlurProcessorConfig config = makeConfig();
        config.maximumExtrapolationMsec = maximumExtrapolationMsec;
        BlurProcessor processor(config);

        GstBuffer* buffer = createPatternBuffer(info);
        if (!buffer) {
            return;
        }

        // PTS 0을 현재 시각에 묶는다. 이후 apply()의 조회 시각은 대략 지금이고,
        // 최신 metadata는 그보다 100 ms 뒤처져 있다
        processor.observeVideoBuffer(buffer);
        const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();

        const auto submitBox = [&](qint64 sourceTimestamp, const QRectF& normalizedBox) {
            BlurFrameData metadata;
            metadata.channelIndex = 0;
            metadata.sourceTimestamp = sourceTimestamp;
            BlurRegionData region;
            region.id = 1;
            region.targetType = BlurTargetType::Face;
            region.normalizedBox = normalizedBox;
            metadata.regions.append(region);
            processor.submitFrame(metadata);
        };
        submitBox(nowMsec - 200, earlierBox);
        submitBox(nowMsec - 100, latestBox);

        GstVideoFrame frame;
        if (gst_video_frame_map(&frame, &info, buffer, GST_MAP_READWRITE) == TRUE) {
            std::vector<guint8> before;
            before.reserve(frameWidth);
            for (int x = 0; x < frameWidth; ++x) {
                before.push_back(lumaAt(frame, x, probeRow));
            }

            processor.apply(frame);

            for (int x = 0; x < frameWidth; ++x) {
                if (lumaAt(frame, x, probeRow) != before[static_cast<size_t>(x)]) {
                    first = std::min(first, x);
                    last = std::max(last, x);
                }
            }
            gst_video_frame_unmap(&frame);
        }

        gst_buffer_unref(buffer);
    };

    int heldFirst = 0;
    int heldLast = 0;
    blurredSpan(0, heldFirst, heldLast);
    int predictedFirst = 0;
    int predictedLast = 0;
    blurredSpan(300, predictedFirst, predictedLast);

    check(heldLast > heldFirst, "the held mask must cover a span on the probe row");
    check(predictedLast > predictedFirst, "the predicted mask must cover a span on the probe row");
    if (heldLast <= heldFirst || predictedLast <= predictedFirst) {
        return;
    }

    // 예측은 마지막 위치가 아니라 예측 위치를 덮는다. 합집합이면 절반만 앞선다
    const double heldCenter = (heldFirst + heldLast) / 2.0;
    const double predictedCenter = (predictedFirst + predictedLast) / 2.0;
    check(predictedCenter - heldCenter > expectedShiftPixels * 0.75,
          "the predicted mask must move ahead by the full measured motion");

    // 진행 방향 앞쪽은 예측이 있어야만 덮인다
    check(predictedLast > heldLast + expectedShiftPixels * 0.5,
          "extrapolation must cover the leading edge of a moving object");
}
}  // namespace

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    checkNv12RegionBlur();
    checkScratchReuseIsStable();
    checkDisabledTargetsLeaveFrame();
    checkExtrapolationCoversMovingObject();

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("BlurProcessor NV12 checks passed\n");
    return 0;
}
