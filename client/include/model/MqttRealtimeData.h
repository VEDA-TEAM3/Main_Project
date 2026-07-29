#pragma once

#include <QMetaType>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include "model/DeviceStatus.h"
#include "model/DigitalTwinTypes.h"

struct RiskObjectData {
    qint64 globalId = 0;
    QString objectClass;
    QPointF worldPosition;
    DigitalTwinRiskLevel riskLevel = DigitalTwinRiskLevel::Normal;
    qint64 nearestId = 0;
    double distance = -1.0;
};

struct RiskFrameData {
    qint64 sourceTimestamp = 0;
    DigitalTwinRiskLevel riskLevel = DigitalTwinRiskLevel::Normal;
    QVector<RiskObjectData> objects;
};

enum class BlurTargetType {
    Face,
    LicensePlate,
};

struct BlurRegionData {
    qint64 id = 0;
    BlurTargetType targetType = BlurTargetType::Face;
    QRectF normalizedBox;
};

struct BlurFrameData {
    int channelIndex = -1;
    qint64 sourceTimestamp = 0;
    QVector<BlurRegionData> regions;
};

struct CentralEventData {
    int channelIndex = -1;
    qint64 sourceTimestamp = 0;
    QString eventType;
    bool active = false;
    int severity = 0;
    QString eventId;
    QString source;
    bool hardwareOk = false;
    QString detail;
    DeviceOutputState hardwareState;
};

Q_DECLARE_METATYPE(RiskObjectData)
Q_DECLARE_METATYPE(RiskFrameData)
Q_DECLARE_METATYPE(BlurTargetType)
Q_DECLARE_METATYPE(BlurRegionData)
Q_DECLARE_METATYPE(BlurFrameData)
Q_DECLARE_METATYPE(CentralEventData)
