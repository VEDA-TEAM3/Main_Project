#pragma once

enum class VideoPreprocessingPreset {
    Custom,
    Day,
    Night,
};

struct VideoPreprocessingSettings {
    bool enabled = true;
    VideoPreprocessingPreset preset = VideoPreprocessingPreset::Custom;
    int brightness = 0;
    double contrast = 1.0;
    double gamma = 1.0;
    bool weakDenoiseEnabled = false;
    bool weakSharpeningEnabled = false;
};
