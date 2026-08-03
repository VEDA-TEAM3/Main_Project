#pragma once

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <optional>

#include "model/DigitalTwinRuntimeConfig.h"
#include "model/DigitalTwinTypes.h"
#include "model/MqttRealtimeData.h"
#include "model/VideoFrameTimestamp.h"

class RiskObjectTracker final {
public:
    explicit RiskObjectTracker(DigitalTwinRuntimeConfig config = {});

    void reset();
    bool submitFrame(RiskFrameData frame, qint64 arrivalTimeMsec);
    bool expireStaleFrame(qint64 currentTimeMsec, qint64 expiryMsec);
    bool hasFrame() const;
    DigitalTwinSnapshot buildSnapshot(qint64 localTimeMsec,
                                      const std::optional<VideoFrameTimestamp>& videoTimestamp = std::nullopt);
    QVector<DigitalTwinRiskEvent> takeRiskEvents();

private:
    RiskFrameData interpolatedFrame(qint64 sourceTimestamp) const;
    void updateAutomaticWorldBounds(const RiskFrameData& frame);
    bool worldBoundsReady() const;
    QPointF normalizedWorldPosition(const QPointF& worldPosition) const;
    QPointF stabilizedPosition(const QString& objectId, const QPointF& measuredPosition, qint64 localTimeMsec);
    qreal lifecycleOpacity(qint64 objectId, bool present, qint64 missingAgeMsec, qint64 localTimeMsec);
    void removeInactivePositionStates(const QHash<QString, QPointF>& currentPositions);

    QVector<RiskFrameData> history_;
    QHash<qint64, RiskObjectData> retainedObjects_;
    QHash<qint64, qint64> lastSeenSourceTimes_;
    QHash<qint64, qreal> renderedOpacities_;
    QHash<qint64, qint64> opacityUpdateTimesMsec_;
    QHash<QString, QPointF> previousPositions_;
    QHash<QString, QPointF> stabilizedPositions_;
    QHash<QString, qint64> stabilizedPositionTimesMsec_;
    QHash<QString, DigitalTwinRiskLevel> previousPairRiskLevels_;
    QHash<QString, qint64> nextPairPulseTimesMsec_;
    QVector<DigitalTwinRiskEvent> pendingRiskEvents_;
    QVector<qint64> sourceClockOffsetSamples_;
    DigitalTwinRuntimeConfig config_;
    QRectF configuredWorldBounds_;
    QRectF automaticWorldBounds_;
    QVector<QPointF> automaticWorldSamples_;
    qint64 automaticBoundsStartSourceTimestamp_ = 0;
    qint64 lastArrivalTimeMsec_ = 0;
    qint64 sourceClockOffsetMsec_ = 0;
    qint64 lastRenderSourceTimestamp_ = 0;
    qint64 lastDiagnosticsMsec_ = 0;
    bool hasConfiguredWorldBounds_ = false;
    bool hasAutomaticWorldBounds_ = false;
    bool invertWorldY_ = true;
};
