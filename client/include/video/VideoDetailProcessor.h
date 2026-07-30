#pragma once

#include <gst/video/video-frame.h>

#include <atomic>
#include <vector>

#include "model/VideoPreprocessingSettings.h"

class VideoDetailProcessor final {
public:
    void setSettings(const VideoPreprocessingSettings& settings);
    void apply(GstVideoFrame& frame);

private:
    std::atomic_bool denoiseEnabled_{false};
    std::atomic_bool sharpeningEnabled_{false};
    std::vector<guint8> sourcePixels_;
};
