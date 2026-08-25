#pragma once

#include <QMetaType>
#include <QString>

#include "model/DeviceStatus.h"

enum class DeviceStatusReportType {
    ChannelStatusSnapshot,
    ControllerOnline,
    SensorOnline,
    SensorOffline,
    FeedbackConfirmed,
    FeedbackAcknowledged,
    FeedbackFailed,
    ProtocolError,
};

struct DeviceStatusReport {
    DeviceStatusReportType type = DeviceStatusReportType::ProtocolError;
    int channelIndex = -1;
    qint64 sourceTimestamp = 0;
    QString node;
    QString detail;
    bool cameraAlive = false;
    bool hardwareAlive = false;
    bool hasOutputState = false;
    DeviceOutputState outputs;
};

Q_DECLARE_METATYPE(DeviceStatusReport)
