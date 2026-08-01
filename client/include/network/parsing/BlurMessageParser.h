#pragma once

#include <QByteArray>
#include <QString>

#include "model/MqttRealtimeData.h"

class BlurMessageParser final {
public:
    static bool parse(const QByteArray& payload, const QString& topic, int topicWireChannel, BlurFrameData& frame,
                      QString& error);
};
