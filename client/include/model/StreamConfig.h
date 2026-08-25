#pragma once

#include <QString>

struct StreamConfig {
    QString cameraId;
    QString name;
    QString url;
    int channelIndex = 0;
    bool enabled = true;
};
