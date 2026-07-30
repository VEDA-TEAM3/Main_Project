#pragma once

#include <gst/gst.h>

class VideoDetailProcessor;

class VideoDetailFilter final {
public:
    static bool ensureRegistered();
    static void setProcessor(GstElement* element, VideoDetailProcessor* processor);
};
