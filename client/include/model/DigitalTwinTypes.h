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
    int channelIndex = -1;
    DigitalTwinObjectType type = DigitalTwinObjectType::Vehicle;
    QPointF position;
    QPointF velocity;
    QColor color;
    DigitalTwinRiskLevel riskLevel = DigitalTwinRiskLevel::Normal;
    qreal opacity = 1.0;
    bool observed = true;
};

struct DigitalTwinRiskEvent {
    QString firstObjectId;
    QString secondObjectId;
    QPointF position;
    DigitalTwinRiskLevel riskLevel = DigitalTwinRiskLevel::Normal;
    int channelIndex = -1;
};

struct DigitalTwinPairRiskState {
    QString firstObjectId;
    QString secondObjectId;
    DigitalTwinRiskLevel riskLevel = DigitalTwinRiskLevel::Normal;
};

struct DigitalTwinSnapshot {
    QVector<DigitalTwinObject> objects;
    QVector<DigitalTwinPairRiskState> pairRiskStates;
    /// 이 스냅샷이 몇 번째 수신 샘플에서 나왔는지. 렌더 보간 프레임은 직전 값을 그대로 물려받으므로
    /// 지도는 이 값이 바뀐 프레임에만 이동 경로 점을 남긴다
    qint64 sampleSequence = 0;
};

Q_DECLARE_METATYPE(DigitalTwinObject)
Q_DECLARE_METATYPE(DigitalTwinRiskLevel)
Q_DECLARE_METATYPE(DigitalTwinRiskEvent)
Q_DECLARE_METATYPE(DigitalTwinPairRiskState)
Q_DECLARE_METATYPE(DigitalTwinSnapshot)
Q_DECLARE_METATYPE(QVector<DigitalTwinObject>)
Q_DECLARE_METATYPE(QVector<DigitalTwinRiskLevel>)
