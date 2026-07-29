#pragma once

#include <QtGlobal>

#include "network/MqttConnectionConfig.h"
#include "network/MqttSubscription.h"

struct MqttTopicsConfig {
    MqttSubscription controllerStatus;
    MqttSubscription centralStatus;
    MqttSubscription sensorAlive;
    MqttSubscription centralEvent;
    MqttSubscription risk;
    MqttSubscription blur;
};

struct MqttDispatcherConfig {
    int blurFlushIntervalMsec = 0;
    int blurMaximumPendingFramesPerChannel = 0;
    int blurSourceRestartGapMsec = 0;
    qint64 blurTimestampRestartThresholdMsec = 0;
    int riskFlushIntervalMsec = 0;
    int riskSourceRestartGapMsec = 0;
    qint64 riskTimestampRollbackResetMsec = 0;
};

struct MqttRuntimeConfig {
    MqttConnectionConfig connection;
    MqttTopicsConfig topics;
    MqttDispatcherConfig dispatcher;
    int blurDebugLogIntervalMsec = 0;
    int riskDebugLogIntervalMsec = 0;
    qsizetype maximumDebugPayloadLength = 0;
};
