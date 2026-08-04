#pragma once

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include "model/DigitalTwinRuntimeConfig.h"
#include "model/DigitalTwinTypes.h"
#include "model/MqttRealtimeData.h"

class RiskObjectTracker final {
public:
    explicit RiskObjectTracker(DigitalTwinRuntimeConfig config = {});

    void reset();
    bool submitFrame(RiskFrameData frame, qint64 arrivalTimeMsec);
    bool expireStaleFrame(qint64 currentTimeMsec, qint64 expiryMsec);
    bool hasFrame() const;
    DigitalTwinSnapshot buildSnapshot(qint64 localTimeMsec);
    QVector<DigitalTwinRiskEvent> takeRiskEvents();

private:
    /**
     * @brief VEDA_TOPVIEW_DEBUG로 켜는 좌표 진단 상태
     *
     * @details level 1은 1초 요약, level 2는 객체별 프레임 상세까지 남긴다.
     *          꺼져 있으면(기본값) 아무 비용도 들지 않도록 모든 경로가 level_로 먼저 걸러진다.
     */
    struct Diagnostics {
        int level = 0;
        int detailIntervalMsec = 1000;
        qint64 windowStartMsec = 0;
        qint64 previousSourceTimestamp = 0;
        int frameCount = 0;
        int duplicateCount = 0;
        int backwardTimestampCount = 0;
        qint64 minTimestampDeltaMsec = 0;
        qint64 maximumTimestampDeltaMsec = 0;
        int rateLimitedCount = 0;
        int medianRejectedCount = 0;
        QVector<qint64> arrivalIntervalsMsec;
        QHash<qint64, QPointF> previousRawPositions;
        QHash<qint64, qint64> lastObjectLogMsec;
    };

    /** @brief 쌍별 위험 파동 발생 상태 (레벨, 마지막 발생·관측 시각) */
    struct PairPulseState {
        DigitalTwinRiskLevel riskLevel = DigitalTwinRiskLevel::Normal;
        qint64 lastPulseMsec = 0;
        qint64 lastSeenMsec = 0;
    };

    struct PositionTransitionState {
        QPointF startPosition;
        QPointF targetPosition;
        qint64 transitionStartMsec = 0;
        qint64 targetFrameSequence = 0;
    };

    void updateAutomaticWorldBounds(const RiskFrameData& frame);
    void expandAutomaticWorldBounds(const RiskFrameData& frame);
    void logAutomaticWorldBounds(const QString& reason) const;
    bool worldBoundsReady() const;
    QPointF normalizedWorldPosition(const QPointF& worldPosition) const;
    QPointF medianFilteredWorldPosition(qint64 globalId, const QPointF& worldPosition);
    int channelIndexForObject(qint64 globalId, const QPointF& normalizedPosition);
    QPointF rateLimitedWorldPosition(qint64 globalId, const QPointF& worldPosition, qint64 arrivalTimeMsec);
    void logRateLimitedJump(qint64 globalId, double distance, double maximumDistance, qint64 localTimeMsec);
    void logFrameDiagnostics(const RiskFrameData& frame, const QVector<QPointF>& rawPositions,
                             const QVector<QPointF>& medianPositions, qint64 arrivalTimeMsec);
    void logDiagnosticsSummary(qint64 arrivalTimeMsec, qsizetype objectCount);
    QString worldBoundsDescription() const;
    QPointF transitionedPosition(const QString& objectId, const QPointF& targetPosition, qint64 frameSequence,
                                 qint64 localTimeMsec);
    qreal lifecycleOpacity(qint64 objectId, bool present, qint64 missingAgeMsec, qint64 localTimeMsec);
    void removeInactivePositionStates(const QHash<QString, QPointF>& currentPositions, qint64 localTimeMsec);

    QVector<RiskFrameData> history_;
    QHash<qint64, RiskObjectData> retainedObjects_;
    QHash<qint64, QVector<QPointF>> worldPositionHistories_;
    QHash<qint64, int> channelIndexes_;
    QHash<qint64, qint64> lastSeenArrivalTimesMsec_;
    QHash<qint64, qreal> renderedOpacities_;
    QHash<qint64, qint64> opacityUpdateTimesMsec_;
    QHash<QString, QPointF> previousPositions_;
    QHash<qint64, QPointF> filteredPositions_;
    QHash<qint64, qint64> filteredPositionTimesMsec_;
    QHash<QString, PositionTransitionState> positionTransitions_;
    QHash<QString, PairPulseState> pairPulseStates_;
    QVector<DigitalTwinRiskEvent> pendingRiskEvents_;
    DigitalTwinRuntimeConfig config_;
    QRectF configuredWorldBounds_;
    QRectF automaticWorldBounds_;
    QVector<QPointF> automaticWorldSamples_;
    QRectF pendingExpansionBounds_;
    qint64 automaticBoundsStartSourceTimestamp_ = 0;
    int pendingExpansionFrameCount_ = 0;
    Diagnostics diagnostics_;
    qint64 frameSequence_ = 0;
    qint64 lastArrivalTimeMsec_ = 0;
    qint64 lastDiagnosticsMsec_ = 0;
    qint64 lastRateLimitLogMsec_ = 0;
    qint64 lastPulseEmitMsec_ = 0;
    bool hasConfiguredWorldBounds_ = false;
    bool hasAutomaticWorldBounds_ = false;
    bool invertWorldY_ = true;
};
