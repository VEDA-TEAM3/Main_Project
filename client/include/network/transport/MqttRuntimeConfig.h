#pragma once

#include <QtGlobal>

#include "network/transport/MqttConnectionConfig.h"
#include "network/transport/MqttSubscription.h"

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
    int blurSourceRestartGapMsec = 0;
    qint64 blurTimestampRestartThresholdMsec = 0;
    int riskFlushIntervalMsec = 0;
    int riskSourceRestartGapMsec = 0;
    bool logBlurDispatch = false;
    bool logRiskDispatch = false;
};

struct MqttRuntimeConfig {
    MqttConnectionConfig connection;
    MqttTopicsConfig topics;
    MqttDispatcherConfig dispatcher;
    int channelCount = 0;
    int blurDebugLogIntervalMsec = 0;
    int riskDebugLogIntervalMsec = 0;
    qsizetype maximumDebugPayloadLength = 0;
    bool logStatusPayload = false;
    bool logRisk = false;
    bool logBlur = false;
};
