#pragma once

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QVector>

#include "model/DigitalTwinTypes.h"
#include "model/MqttRealtimeData.h"

class RiskObjectTracker final {
public:
    RiskObjectTracker();

    void reset();
    bool submitFrame(RiskFrameData frame, qint64 arrivalTimeMsec);
    bool expireStaleFrame(qint64 currentTimeMsec, qint64 expiryMsec);
    bool hasFrame() const;
    DigitalTwinSnapshot buildSnapshot(qint64 localTimeMsec);
    QVector<DigitalTwinRiskEvent> takeRiskEvents();

private:
    RiskFrameData interpolatedFrame(qint64 sourceTimestamp) const;
    void updateAutomaticWorldBounds(const RiskFrameData& frame);
    QPointF normalizedWorldPosition(const QPointF& worldPosition) const;

    QVector<RiskFrameData> history_;
    QHash<qint64, RiskObjectData> retainedObjects_;
    QHash<qint64, qint64> lastSeenSourceTimes_;
    QHash<QString, QPointF> previousPositions_;
    QHash<QString, DigitalTwinRiskLevel> previousPairRiskLevels_;
    QHash<QString, qint64> nextPairPulseTimesMsec_;
    QVector<DigitalTwinRiskEvent> pendingRiskEvents_;
    QRectF configuredWorldBounds_;
    QRectF automaticWorldBounds_;
    qint64 lastArrivalTimeMsec_ = 0;
    qint64 sourceClockOffsetMsec_ = 0;
    qint64 lastRenderSourceTimestamp_ = 0;
    bool hasConfiguredWorldBounds_ = false;
    bool hasAutomaticWorldBounds_ = false;
    bool invertWorldY_ = true;
};
