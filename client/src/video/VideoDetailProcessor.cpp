#include "video/VideoDetailProcessor.h"

#include <algorithm>
#include <cstring>

/**
 * @brief          약한 노이즈 제거와 선명도 보정 활성 상태를 갱신합니다.
 * @param settings 영상 전처리 설정
 */
void VideoDetailProcessor::setSettings(const VideoPreprocessingSettings& settings) {
    const bool enabled = settings.enabled;
    denoiseEnabled_.store(enabled && settings.weakDenoiseEnabled, std::memory_order_release);
    sharpeningEnabled_.store(enabled && settings.weakSharpeningEnabled, std::memory_order_release);
}

/**
 * @brief       BGRA 프레임에 약한 공간 필터를 적용합니다.
 * @param frame 쓰기 가능한 BGRA 영상 프레임
 */
void VideoDetailProcessor::apply(GstVideoFrame& frame) {
    const bool denoiseEnabled = denoiseEnabled_.load(std::memory_order_acquire);
    const bool sharpeningEnabled = sharpeningEnabled_.load(std::memory_order_acquire);
    if (!denoiseEnabled && !sharpeningEnabled) {
        return;
    }

    auto* destination = static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    const int width = GST_VIDEO_FRAME_WIDTH(&frame);
    const int height = GST_VIDEO_FRAME_HEIGHT(&frame);
    if (!destination || stride <= 0 || width < 3 || height < 3) {
        return;
    }

    const std::size_t frameBytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    sourcePixels_.resize(frameBytes);
    std::memcpy(sourcePixels_.data(), destination, frameBytes);

    const auto* source = sourcePixels_.data();
    for (int y = 1; y < height - 1; ++y) {
        auto* outputRow = destination + y * stride;
        const auto* sourceRow = source + y * stride;
        const auto* previousRow = source + (y - 1) * stride;
        const auto* nextRow = source + (y + 1) * stride;

        for (int x = 1; x < width - 1; ++x) {
            const int pixelOffset = x * 4;
            for (int component = 0; component < 3; ++component) {
                const int center = sourceRow[pixelOffset + component];
                const int neighbourAverage =
                    (center * 4 + sourceRow[pixelOffset - 4 + component] + sourceRow[pixelOffset + 4 + component] +
                     previousRow[pixelOffset + component] + nextRow[pixelOffset + component]) /
                    8;

                int value = denoiseEnabled ? (center + neighbourAverage) / 2 : center;
                if (sharpeningEnabled) {
                    value += (center - neighbourAverage) / 2;
                }

                outputRow[pixelOffset + component] = static_cast<guint8>(std::clamp(value, 0, 255));
            }
        }
    }
}
