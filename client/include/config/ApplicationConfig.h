#pragma once

#include <QString>

#include "model/DigitalTwinRuntimeConfig.h"
#include "network/transport/MqttRuntimeConfig.h"
#include "video/VideoRuntimeConfig.h"

struct ApplicationWindowConfig {
    int width = 0;
    int height = 0;
};

struct ApplicationConfig {
    ApplicationWindowConfig window;
    DigitalTwinRuntimeConfig digitalTwin;
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
