#pragma once

#include <gst/gst.h>

#include "model/VideoPreprocessingSettings.h"

/**
 * @brief 밝기·대비·감마를 한 번의 룩업으로 처리하는 영상 필터 등록 도우미입니다.
 *
 * @details 예전에는 GStreamer의 videobalance와 gamma 두 요소를 이어 붙였습니다. 세 보정이
 *          모두 8비트 Y에 대한 순수 per-pixel 함수라 표 하나로 합쳐지므로, 평면을 두 번
 *          훑던 것을 한 번으로 줄였습니다. 값은 예전 두 요소와 완전히 같습니다
 *          (tests/VideoPreprocessLutCheck.cpp가 실제 요소와 대조합니다).
 */
class VideoPreprocessFilter final {
public:
    static bool ensureRegistered();
    static void setSettings(GstElement* element, const VideoPreprocessingSettings& settings);
};
