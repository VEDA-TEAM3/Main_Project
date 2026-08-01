#pragma once

#include <QString>

struct MqttConnectionConfig {
    QString host;
    quint16 port = 0;
    QString caCertificatePath;
    QString clientId;
    int keepAliveSeconds = 0;
    int reconnectIntervalMsec = 0;
    bool debugLogging = false;
};
