#pragma once

#include <gst/gst.h>

class BlurProcessor;

/**
 * @brief GStreamer 파이프라인에 머리 블러 처리기를 연결하는 영상 필터 등록 도우미입니다.
 */
class BlurVideoFilter final {
public:
    static bool ensureRegistered();
    static void setProcessor(GstElement* element, BlurProcessor* processor);
};
