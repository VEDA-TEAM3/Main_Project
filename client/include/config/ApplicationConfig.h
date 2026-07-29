#pragma once

#include <QString>

#include "network/MqttRuntimeConfig.h"
#include "video/VideoRuntimeConfig.h"

struct ApplicationWindowConfig {
    int width = 0;
    int height = 0;
};

struct ApplicationConfig {
    ApplicationWindowConfig window;
    VideoRuntimeConfig video;
    MqttRuntimeConfig mqtt;
};

struct ApplicationConfigLoadResult {
    ApplicationConfig config;
    QString sourcePath;
    QString error;
    bool successful = false;
};

class ApplicationConfigLoader final {
public:
    static ApplicationConfigLoadResult load();
};
