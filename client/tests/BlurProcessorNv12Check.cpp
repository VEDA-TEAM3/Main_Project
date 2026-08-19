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
 * @brief 두 metadata 사이의 영역 보간이 정렬된 이진 탐색으로도 짝을 찾는지 검사합니다.
 *
 * @details 짝 찾기가 깨지면 보간만 조용히 멈추고 블러 자체는 계속 그려진다. 화면은 그럴듯하니
 *          눈으로는 알 수 없다. 그래서 일부러 id를 뒤섞어 넣고, 두 시각의 중간에서 블러가
 *          가운데에 찍히는지(양 끝은 그대로인지) 본다.
 */
void checkInterpolationMatchesRegionsById() {
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, frameWidth, frameHeight);

    BlurProcessor processor(makeConfig());
    GstBuffer* buffer = createPatternBuffer(info);
    check(buffer != nullptr, "NV12 buffer must be allocated");
    if (!buffer) {
        return;
    }

    processor.observeVideoBuffer(buffer);
    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();

    // 세로 줄무늬 위를 가로지르도록 y는 고정하고 x만 옮긴다.
    // 중심 x는 100 -> 220 픽셀, 중간은 160이다
    constexpr double boxSize = 0.12;
    constexpr double boxTop = 0.40;
    const auto boxAtCenterX = [](double centerX) { return QRectF(centerX - boxSize / 2.0, boxTop, boxSize, boxSize); };

    // id를 일부러 뒤섞어 넣는다. 정렬/이진 탐색이 잘못되면 여기서 짝을 놓친다.
    // 7번만 움직이고 11번과 3번은 줄무늬 밖에 두어 결과에 영향을 주지 않는다
    const auto metadataAt = [&](qint64 timestamp, double probeCenterX) {
        BlurFrameData metadata;
        metadata.channelIndex = 0;
        metadata.sourceTimestamp = timestamp;
        const auto append = [&metadata](qint64 id, const QRectF& box) {
            BlurRegionData region;
            region.id = id;
            region.targetType = BlurTargetType::Face;
            region.normalizedBox = box;
            metadata.regions.append(region);
        };
        append(11, QRectF(0.10, 0.02, boxSize, boxSize));
        append(7, boxAtCenterX(probeCenterX));
        append(3, QRectF(0.10, 0.86, boxSize, boxSize));
        return metadata;
    };

    processor.submitFrame(metadataAt(nowMsec - 100, 100.0 / frameWidth));
    processor.submitFrame(metadataAt(nowMsec + 100, 220.0 / frameWidth));

    GstVideoFrame frame;
    check(gst_video_frame_map(&frame, &info, buffer, GST_MAP_READWRITE) == TRUE, "frame must map read-write");

    // PTS 0은 대략 nowMsec에 대응하므로 두 metadata의 중간 지점이다
    processor.apply(frame);

    constexpr int probeY = 110;
    const guint8 middleLuma = lumaAt(frame, 160, probeY);
    check(middleLuma > backgroundLuma && middleLuma < patternLuma,
          "interpolated region must be blurred at the midpoint between the two timestamps");
    check(lumaAt(frame, 100, probeY) == backgroundLuma, "the earlier position must not be blurred");
    check(lumaAt(frame, 220, probeY) == backgroundLuma, "the later position must not be blurred");

    gst_video_frame_unmap(&frame);
    gst_buffer_unref(buffer);
}

/**
 * @brief 고정소수점 역수가 정수 나눗셈과 완전히 같은 값을 내는지 검사합니다.
 *
 * @details 픽셀당 나눗셈을 곱셈+시프트로 바꿨다. 1이라도 어긋나면 블러 결과가 미묘하게 달라지는데
 *          눈으로는 알 수 없고 기존 검사(값의 범위, 이웃 차이)도 통과한다. 그래서 등식을 직접
 *          확인한다. 오차가 드러나는 곳은 몫의 소수부가 가장 큰 지점, 즉 count의 배수 언저리뿐이라
 *          그 주변만 훑어도 전 구간을 덮는다. 설정이 허용하는 최대 반경(2048)까지 본다.
 */
void checkReciprocalMatchesDivision() {
    constexpr int maximumConfigurableRadius = 2048;
    const int maximumCount = 2 * maximumConfigurableRadius + 1;

    int mismatchCount = 0;
    for (int count = 1; count <= maximumCount; ++count) {
        const quint64 reciprocal = (static_cast<quint64>(1) << blurReciprocalShift) / count + 1;
        const qint64 highestSum = 255LL * count;
        for (int multiple = 0; multiple <= 255; ++multiple) {
            for (int delta = -1; delta <= 1; ++delta) {
                const qint64 candidate = static_cast<qint64>(multiple) * count + delta;
                if (candidate < 0 || candidate > highestSum) {
                    continue;
                }

                const quint64 sum = static_cast<quint64>(candidate);
                if (((sum * reciprocal) >> blurReciprocalShift) != sum / static_cast<quint64>(count)) {
                    ++mismatchCount;
                }
            }
        }
    }

    check(mismatchCount == 0, "fixed-point reciprocal must match integer division exactly");
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
}  // namespace

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    checkNv12RegionBlur();
    checkScratchReuseIsStable();
    checkInterpolationMatchesRegionsById();
    checkReciprocalMatchesDivision();
    checkDisabledTargetsLeaveFrame();

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("BlurProcessor NV12 checks passed\n");
    return 0;
}
