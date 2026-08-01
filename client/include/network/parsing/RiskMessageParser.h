#pragma once

#include <QByteArray>
#include <QString>

#include "model/MqttRealtimeData.h"

class RiskMessageParser final {
public:
    static bool parse(const QByteArray& payload, const QString& topic, RiskFrameData& frame, QString& error);
};
