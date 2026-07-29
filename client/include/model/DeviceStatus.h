#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

enum class DeviceFeedbackHealth {
    Unknown,
    Confirmed,
    Failed,
};

enum class SensorHealth {
    Unknown,
    Online,
    Offline,
};

struct DeviceOutputState {
    bool ledRed = false;
    bool ledYellow = false;
    bool ledGreen = false;
    bool beacon = false;
    bool buzzer = false;
};

struct DeviceChannelStatus {
    int channelIndex = 0;
    DeviceOutputState outputs;
    bool hasConfirmedState = false;
    SensorHealth sensorHealth = SensorHealth::Unknown;
    DeviceFeedbackHealth feedbackHealth = DeviceFeedbackHealth::Unknown;
    QString sensorDetail;
    QString detail;
    qint64 confirmedSourceTimestamp = 0;
};

Q_DECLARE_METATYPE(DeviceOutputState)
Q_DECLARE_METATYPE(DeviceChannelStatus)
Q_DECLARE_METATYPE(QVector<DeviceChannelStatus>)
