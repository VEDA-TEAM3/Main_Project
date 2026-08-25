// 합성 LUT가 예전 파이프라인(videobalance ! gamma)과 값이 같은지 검사한다.
//
// GStreamer 소스에서 수식을 읽어 옮겨 적는 대신, 두 요소를 실제로 돌려 나온 값을 정답으로
// 삼는다. 그래야 요소 구현이 바뀌거나 우리가 수식을 잘못 읽었을 때 여기서 걸린다.
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video-frame.h>
#include <gst/video/video-info.h>

#include <array>
#include <cstdio>
#include <exception>

#include "video/VideoPreprocessLut.h"

namespace {
/// Y = 0..255 를 한 줄에 다 담는다. 높이는 NV12 크로마 평면이 성립하는 최솟값
constexpr int rampWidth = 256;
constexpr int rampHeight = 2;

int failureCount = 0;

void check(bool condition, const char* description) {
    if (condition) {
        return;
    }

    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failureCount;
}

/** @brief Y가 0부터 255까지 1씩 오르는 NV12 프레임을 만듭니다. */
GstBuffer* createRampBuffer(const GstVideoInfo& info) {
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, GST_VIDEO_INFO_SIZE(&info), nullptr);
    GstVideoFrame frame;
    if (!buffer || !gst_video_frame_map(&frame, const_cast<GstVideoInfo*>(&info), buffer, GST_MAP_WRITE)) {
        return buffer;
    }

    auto* luma = static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const int lumaStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    for (int y = 0; y < rampHeight; ++y) {
        for (int x = 0; x < rampWidth; ++x) {
            luma[y * lumaStride + x] = static_cast<guint8>(x);
        }
    }

    auto* chroma = static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
    const int chromaStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
    for (int x = 0; x < rampWidth; ++x) {
        chroma[x] = 128;
    }
    (void)chromaStride;

    gst_video_frame_unmap(&frame);
    GST_BUFFER_PTS(buffer) = 0;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
    return buffer;
}

/**
 * @brief           예전 파이프라인이 Y 값 256개를 어떻게 바꾸는지 실측합니다.
 * @param settings  적용할 전처리 설정
 * @param result    실측된 표
 * @return          파이프라인이 정상 동작하면 true
 */
bool measureLegacyChain(const VideoPreprocessingSettings& settings, VideoPreprocessLut& result) {
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, rampWidth, rampHeight);

    gchar* description = g_strdup_printf(
        "appsrc name=src is-live=false format=time "
        "caps=video/x-raw,format=NV12,width=%d,height=%d,framerate=30/1 ! "
        "videobalance name=balance brightness=%f contrast=%f saturation=1.0 ! "
        "gamma name=gammafilter gamma=%f ! "
        "appsink name=sink sync=false max-buffers=1",
        rampWidth, rampHeight, static_cast<double>(settings.brightness) / 100.0, settings.contrast, settings.gamma);

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description, &error);
    g_free(description);
    if (!pipeline || error) {
        if (error) {
            g_error_free(error);
        }
        if (pipeline) {
            gst_object_unref(pipeline);
        }
        return false;
    }

    GstElement* source = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    gst_app_src_push_buffer(GST_APP_SRC(source), createRampBuffer(info));
    gst_app_src_end_of_stream(GST_APP_SRC(source));

    bool measured = false;
    if (GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink))) {
        GstVideoFrame frame;
        if (gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
            const auto* luma = static_cast<const guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
            for (int input = 0; input < 256; ++input) {
                result[static_cast<size_t>(input)] = luma[input];
            }
            gst_video_frame_unmap(&frame);
            measured = true;
        }
        gst_sample_unref(sample);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return measured;
}

/**
 * @brief             한 설정에 대해 합성 LUT와 예전 파이프라인의 값이 같은지 확인합니다.
 * @param brightness  -100~100 눈금
 * @param contrast    0~2
 * @param gamma       0.01~10
 * @param label       실패 메시지에 붙일 이름
 */
void checkMatchesLegacyChain(int brightness, double contrast, double gamma, const char* label) {
    VideoPreprocessingSettings settings;
    settings.enabled = true;
    settings.brightness = brightness;
    settings.contrast = contrast;
    settings.gamma = gamma;

    VideoPreprocessLut measured{};
    if (!measureLegacyChain(settings, measured)) {
        std::fprintf(stderr, "FAIL: legacy chain did not run for %s\n", label);
        ++failureCount;
        return;
    }

    const VideoPreprocessLut built = buildVideoPreprocessLut(settings);
    int mismatches = 0;
    int firstInput = -1;
    for (int input = 0; input < 256; ++input) {
        if (built[static_cast<size_t>(input)] != measured[static_cast<size_t>(input)]) {
            if (firstInput < 0) {
                firstInput = input;
            }
            ++mismatches;
        }
    }

    if (mismatches != 0) {
        std::fprintf(stderr, "FAIL: %s differs at %d values (first Y=%d: built=%d legacy=%d)\n", label, mismatches,
                     firstInput, static_cast<int>(built[static_cast<size_t>(firstInput)]),
                     static_cast<int>(measured[static_cast<size_t>(firstInput)]));
        ++failureCount;
    }
}

/** @brief 항등 설정은 표가 그대로여야 하고 passthrough로 판정돼야 합니다. */
void checkNeutralIsIdentity() {
    VideoPreprocessingSettings settings;
    check(isNeutralVideoPreprocessing(settings), "default settings must be neutral");

    settings.enabled = false;
    settings.brightness = 40;
    settings.gamma = 2.0;
    check(isNeutralVideoPreprocessing(settings), "disabled preprocessing must be neutral whatever the values are");

    VideoPreprocessingSettings identity;
    const VideoPreprocessLut lut = buildVideoPreprocessLut(identity);
    bool same = true;
    for (int input = 0; input < 256; ++input) {
        same = same && lut[static_cast<size_t>(input)] == static_cast<unsigned char>(input);
    }
    check(same, "a neutral table must map every value to itself");
}
}  // namespace

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    try {
        checkNeutralIsIdentity();

        // 실제 프리셋
        checkMatchesLegacyChain(3, 1.08, 1.0, "Day preset");
        checkMatchesLegacyChain(10, 1.05, 1.2, "Night preset");

        // 설정 로더가 허용하는 범위의 경계와 그 사이
        checkMatchesLegacyChain(0, 1.0, 1.0, "neutral");
        checkMatchesLegacyChain(100, 2.0, 10.0, "upper bounds");
        checkMatchesLegacyChain(-100, 0.0, 0.01, "lower bounds");
        checkMatchesLegacyChain(50, 1.8, 2.5, "bright and steep");
        checkMatchesLegacyChain(-40, 0.5, 0.4, "dark and flat");
        checkMatchesLegacyChain(0, 1.0, 2.2, "gamma only");
        checkMatchesLegacyChain(25, 1.0, 1.0, "brightness only");
        checkMatchesLegacyChain(0, 1.5, 1.0, "contrast only");

        // 반올림 경계가 갈리는 자리를 훑는다. .5 가 자주 나오는 대비 값들이다
        checkMatchesLegacyChain(1, 1.25, 1.0, "rounding sweep 1");
        checkMatchesLegacyChain(2, 1.5, 1.0, "rounding sweep 2");
        checkMatchesLegacyChain(-1, 0.75, 1.0, "rounding sweep 3");
        checkMatchesLegacyChain(7, 1.02, 1.35, "rounding sweep 4");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "checks aborted: %s\n", error.what());
        return 1;
    }

    if (failureCount > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failureCount);
        return 1;
    }

    std::printf("VideoPreprocessLut checks passed\n");
    return 0;
}
