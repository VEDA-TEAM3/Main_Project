#pragma once

#include <QString>

struct MqttSubscription {
    QString topicFilter;
    quint8 qos = 0;
};
