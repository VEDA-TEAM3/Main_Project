#pragma once

#include <QColor>
#include <QMetaType>
#include <QPointF>
#include <QString>
#include <QVector>

enum class DigitalTwinObjectType {
    Vehicle,
    Pedestrian,
};

enum class DigitalTwinRiskLevel {
    Normal,
    Warning,
    Danger,
};

struct DigitalTwinObject {
    QString objectId;
    int channelIndex = 0;
    DigitalTwinObjectType type = DigitalTwinObjectType::Vehicle;
    QPointF position;
    QPointF velocity;
    QColor color;
    DigitalTwinRiskLevel riskLevel = DigitalTwinRiskLevel::Normal;
};

struct DigitalTwinRiskEvent {
    QString firstObjectId;
    QString secondObjectId;
    QPointF position;
    DigitalTwinRiskLevel riskLevel = DigitalTwinRiskLevel::Normal;
};

struct DigitalTwinPairRiskState {
    QString firstObjectId;
    QString secondObjectId;
    DigitalTwinRiskLevel riskLevel = DigitalTwinRiskLevel::Normal;
};

struct DigitalTwinSnapshot {
    QVector<DigitalTwinObject> objects;
    QVector<DigitalTwinPairRiskState> pairRiskStates;
};

Q_DECLARE_METATYPE(DigitalTwinObject)
Q_DECLARE_METATYPE(DigitalTwinRiskLevel)
Q_DECLARE_METATYPE(DigitalTwinRiskEvent)
Q_DECLARE_METATYPE(DigitalTwinPairRiskState)
Q_DECLARE_METATYPE(DigitalTwinSnapshot)
Q_DECLARE_METATYPE(QVector<DigitalTwinObject>)
Q_DECLARE_METATYPE(QVector<DigitalTwinRiskLevel>)
