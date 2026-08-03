#include "model/RiskObjectTracker.h"

#include <QDebug>
#include <QHash>
#include <QProcessEnvironment>
#include <QSet>
#include <algorithm>
#include <iterator>
#include <utility>

namespace {
constexpr qint64 sourceRestartGapMsec = 5000;
constexpr qint64 sourceTimestampRollbackResetMsec = 1000;
constexpr qint64 warningPulseRepeatMsec = 1200;
constexpr qint64 dangerPulseRepeatMsec = 1500;
constexpr qint64 positionFilterResetGapMsec = 500;
constexpr qint64 minimumPositionFilterStepMsec = 16;
constexpr qint64 maximumPositionFilterStepMsec = 100;
constexpr double maximumNormalizedSpeedPerSecond = 0.9;
constexpr qsizetype maximumClockOffsetSampleCount = 15;

qint64 pulseRepeatMsec(DigitalTwinRiskLevel riskLevel) {
    return riskLevel == DigitalTwinRiskLevel::Danger ? dangerPulseRepeatMsec : warningPulseRepeatMsec;
}

bool readConfiguredWorldBounds(QRectF& bounds) {
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    bool minXOk = false;
    bool minYOk = false;
    bool maxXOk = false;
    bool maxYOk = false;
    const double minX = environment.value(QStringLiteral("VEDA_MAP_MIN_X")).toDouble(&minXOk);
    const double minY = environment.value(QStringLiteral("VEDA_MAP_MIN_Y")).toDouble(&minYOk);
    const double maxX = environment.value(QStringLiteral("VEDA_MAP_MAX_X")).toDouble(&maxXOk);
    const double maxY = environment.value(QStringLiteral("VEDA_MAP_MAX_Y")).toDouble(&maxYOk);

    if (!minXOk || !minYOk || !maxXOk || !maxYOk || maxX <= minX || maxY <= minY) {
        return false;
    }

    bounds = QRectF(minX, minY, maxX - minX, maxY - minY);
    return true;
}

bool readInvertWorldY() {
    const QString value = QProcessEnvironment::systemEnvironment()
                              .value(QStringLiteral("VEDA_MAP_INVERT_Y"), QStringLiteral("1"))
                              .trimmed()
                              .toLower();
    return value != QStringLiteral("0") && value != QStringLiteral("false") && value != QStringLiteral("no");
}

int channelIndexForPosition(const QPointF& position) {
    const int column = position.x() >= 0.5 ? 1 : 0;
    const int row = position.y() >= 0.5 ? 1 : 0;
    return row * 2 + column;
}

QPointF interpolatePosition(const QPointF& first, const QPointF& second, double ratio) {
    return first + (second - first) * ratio;
}
}  // namespace

/** @brief 통합 RiskFrame을 지도 객체 스냅샷으로 변환하는 추적기를 생성합니다. */
RiskObjectTracker::RiskObjectTracker(DigitalTwinRuntimeConfig config) : config_(std::move(config)) {
    if (config_.world.fixedBoundsEnabled && config_.world.bounds.width() > 0.0 && config_.world.bounds.height() > 0.0) {
        configuredWorldBounds_ = config_.world.bounds;
        hasConfiguredWorldBounds_ = true;
        invertWorldY_ = config_.world.invertY;
        return;
    }

    hasConfiguredWorldBounds_ = readConfiguredWorldBounds(configuredWorldBounds_);
    invertWorldY_ = hasConfiguredWorldBounds_ ? readInvertWorldY() : config_.world.invertY;
}

/** @brief 수신 이력과 객체 이동 이력을 초기화합니다. */
void RiskObjectTracker::reset() {
    history_.clear();
    retainedObjects_.clear();
    lastSeenSourceTimes_.clear();
    renderedOpacities_.clear();
    opacityUpdateTimesMsec_.clear();
    previousPositions_.clear();
    stabilizedPositions_.clear();
    stabilizedPositionTimesMsec_.clear();
    previousPairRiskLevels_.clear();
    nextPairPulseTimesMsec_.clear();
    pendingRiskEvents_.clear();
    sourceClockOffsetSamples_.clear();
    automaticWorldBounds_ = {};
    lastArrivalTimeMsec_ = 0;
    sourceClockOffsetMsec_ = 0;
    lastRenderSourceTimestamp_ = 0;
    lastDiagnosticsMsec_ = 0;
    hasAutomaticWorldBounds_ = false;
}

/**
 * @brief                  최신 통합 위험 프레임을 시간순 이력에 반영합니다.
 * @param frame            계약 검증을 통과한 RiskFrame
 * @param arrivalTimeMsec  로컬 수신 시각
 * @return                 프레임을 반영하면 true
 */
bool RiskObjectTracker::submitFrame(RiskFrameData frame, qint64 arrivalTimeMsec) {
    if (frame.sourceTimestamp <= 0 || arrivalTimeMsec <= 0) {
        return false;
    }

    const bool arrivalGapDetected =
        lastArrivalTimeMsec_ > 0 && arrivalTimeMsec - lastArrivalTimeMsec_ > sourceRestartGapMsec;
    const bool sourceTimestampRolledBack =
        !history_.isEmpty() &&
        history_.constLast().sourceTimestamp - frame.sourceTimestamp > sourceTimestampRollbackResetMsec;
    if (arrivalGapDetected || sourceTimestampRolledBack) {
        reset();
    }
    lastArrivalTimeMsec_ = arrivalTimeMsec;

    if (!history_.isEmpty() && frame.sourceTimestamp <= history_.constLast().sourceTimestamp) {
        return false;
    }

    const qint64 measuredClockOffsetMsec = arrivalTimeMsec - frame.sourceTimestamp;
    sourceClockOffsetSamples_.append(measuredClockOffsetMsec);
    while (sourceClockOffsetSamples_.size() > maximumClockOffsetSampleCount) {
        sourceClockOffsetSamples_.removeFirst();
    }
    QVector<qint64> sortedOffsets = sourceClockOffsetSamples_;
    std::sort(sortedOffsets.begin(), sortedOffsets.end());
    sourceClockOffsetMsec_ = sortedOffsets[sortedOffsets.size() / 2];

    for (const RiskObjectData& object : frame.objects) {
        retainedObjects_.insert(object.globalId, object);
        lastSeenSourceTimes_.insert(object.globalId, frame.sourceTimestamp);
    }

    const qint64 objectRetentionMsec = config_.missingGraceMsec + config_.fadeOutMsec;
    for (auto iterator = lastSeenSourceTimes_.begin(); iterator != lastSeenSourceTimes_.end();) {
        if (frame.sourceTimestamp - iterator.value() <= objectRetentionMsec) {
            ++iterator;
            continue;
        }

        retainedObjects_.remove(iterator.key());
        iterator = lastSeenSourceTimes_.erase(iterator);
    }

    history_.append(std::move(frame));
    while (history_.size() > config_.maximumHistorySize) {
        history_.removeFirst();
    }
    return true;
}

/**
 * @brief                  지정 시간 동안 갱신되지 않은 통합 위험 프레임을 제거합니다.
 * @param currentTimeMsec  현재 로컬 시각
 * @param expiryMsec       만료 기준
 * @return                 프레임이 제거되면 true
 */
bool RiskObjectTracker::expireStaleFrame(qint64 currentTimeMsec, qint64 expiryMsec) {
    if (history_.isEmpty() || currentTimeMsec - lastArrivalTimeMsec_ <= expiryMsec) {
        return false;
    }

    reset();
    return true;
}

/** @brief 표시할 통합 위험 프레임이 있는지 확인합니다. */
bool RiskObjectTracker::hasFrame() const { return !history_.isEmpty(); }

/**
 * @brief                표시 시각에 맞춰 보간한 통합 객체 스냅샷을 만듭니다.
 * @param localTimeMsec  현재 로컬 시각
 * @return               UI 렌더링용 디지털 트윈 스냅샷
 */
DigitalTwinSnapshot RiskObjectTracker::buildSnapshot(qint64 localTimeMsec,
                                                     const std::optional<VideoFrameTimestamp>& videoTimestamp) {
    pendingRiskEvents_.clear();
    if (history_.isEmpty()) {
        return {};
    }

    updateAutomaticWorldBounds(history_.constLast());
    const qint64 estimatedSourceTimestamp = localTimeMsec - sourceClockOffsetMsec_ - config_.renderDelayMsec;
    qint64 calculatedSourceTimestamp = estimatedSourceTimestamp;
    QString clockSource = QStringLiteral("arrival-offset");
    if (videoTimestamp.has_value() && videoTimestamp->isValid() &&
        localTimeMsec - videoTimestamp->observedLocalMsec <= config_.videoTimestampTimeoutMsec) {
        const qint64 elapsedSinceVideoObservationMsec =
            qMax<qint64>(0, localTimeMsec - videoTimestamp->observedLocalMsec);
        const qint64 progressingVideoUtcMsec = videoTimestamp->utcMsec + elapsedSinceVideoObservationMsec;
        const qint64 videoBasedTimestamp =
            videoTimestamp->senderClock ? progressingVideoUtcMsec - config_.videoSyncCorrectionMsec
                                        : progressingVideoUtcMsec - sourceClockOffsetMsec_ - config_.renderDelayMsec;
        if (qAbs(videoBasedTimestamp - estimatedSourceTimestamp) <= config_.maximumVideoClockSkewMsec) {
            calculatedSourceTimestamp = videoBasedTimestamp;
            clockSource =
                videoTimestamp->senderClock ? QStringLiteral("video-sender-clock") : QStringLiteral("video-pts-anchor");
        } else {
            clockSource = QStringLiteral("video-clock-rejected");
        }
    }

    const qint64 latestSourceTimestamp = history_.constLast().sourceTimestamp;
    const qint64 targetSourceTimestamp =
        qMin(latestSourceTimestamp, qMax(calculatedSourceTimestamp, lastRenderSourceTimestamp_));
    lastRenderSourceTimestamp_ = targetSourceTimestamp;
    RiskFrameData frame = interpolatedFrame(targetSourceTimestamp);

    QHash<qint64, const RiskObjectData*> latestObjects;
    latestObjects.reserve(history_.constLast().objects.size());
    for (const RiskObjectData& object : history_.constLast().objects) {
        latestObjects.insert(object.globalId, &object);
    }
    for (RiskObjectData& object : frame.objects) {
        const auto latestObject = latestObjects.constFind(object.globalId);
        if (latestObject == latestObjects.cend()) {
            continue;
        }
        object.riskLevel = (*latestObject)->riskLevel;
        object.nearestId = (*latestObject)->nearestId;
        object.distance = (*latestObject)->distance;
    }

    if (config_.diagnosticsIntervalMsec > 0 &&
        localTimeMsec - lastDiagnosticsMsec_ >= config_.diagnosticsIntervalMsec) {
        const qint64 videoUtcMsec = videoTimestamp.has_value() ? videoTimestamp->utcMsec : 0;
        const int referenceChannel = videoTimestamp.has_value() ? videoTimestamp->channelIndex + 1 : 0;
        const bool senderClock = videoTimestamp.has_value() && videoTimestamp->senderClock;
        qInfo().noquote() << QStringLiteral(
                                 "[TOPVIEW SYNC] clock=%1 channel=%2 videoTs=%3 senderClock=%4 targetTs=%5 "
                                 "latestRiskTs=%6 buffered=%7ms offset=%8ms history=%9")
                                 .arg(clockSource)
                                 .arg(referenceChannel)
                                 .arg(videoUtcMsec)
                                 .arg(senderClock)
                                 .arg(targetSourceTimestamp)
                                 .arg(latestSourceTimestamp)
                                 .arg(latestSourceTimestamp - targetSourceTimestamp)
                                 .arg(sourceClockOffsetMsec_)
                                 .arg(history_.size());
        lastDiagnosticsMsec_ = localTimeMsec;
    }

    DigitalTwinSnapshot snapshot;
    QHash<QString, QPointF> currentPositions;
    QSet<QString> pairKeys;
    QSet<qint64> includedObjectIds;
    snapshot.objects.reserve(qMax(frame.objects.size(), retainedObjects_.size()));

    const auto appendObject = [this, localTimeMsec, &snapshot, &currentPositions, &pairKeys, &includedObjectIds](
                                  const RiskObjectData& sourceObject, qreal opacity) {
        if (includedObjectIds.contains(sourceObject.globalId)) {
            return;
        }
        includedObjectIds.insert(sourceObject.globalId);

        DigitalTwinObject object;
        object.objectId = QStringLiteral("G-%1").arg(sourceObject.globalId);
        object.type = sourceObject.objectClass == QStringLiteral("Human") ? DigitalTwinObjectType::Pedestrian
                                                                          : DigitalTwinObjectType::Vehicle;
        const QPointF measuredPosition = normalizedWorldPosition(sourceObject.worldPosition);
        object.position = stabilizedPosition(object.objectId, measuredPosition, localTimeMsec);
        object.channelIndex = channelIndexForPosition(object.position);
        object.velocity = object.position - previousPositions_.value(object.objectId, object.position);
        object.riskLevel = sourceObject.riskLevel;
        object.opacity = qBound(0.0, opacity, 1.0);
        snapshot.objects.append(object);
        currentPositions.insert(object.objectId, object.position);

        if (sourceObject.nearestId <= 0 || sourceObject.riskLevel == DigitalTwinRiskLevel::Normal) {
            return;
        }

        const qint64 firstId = qMin(sourceObject.globalId, sourceObject.nearestId);
        const qint64 secondId = qMax(sourceObject.globalId, sourceObject.nearestId);
        const QString pairKey = QStringLiteral("%1:%2").arg(firstId).arg(secondId);
        if (pairKeys.contains(pairKey)) {
            return;
        }
        pairKeys.insert(pairKey);

        DigitalTwinPairRiskState pairState;
        pairState.firstObjectId = QStringLiteral("G-%1").arg(firstId);
        pairState.secondObjectId = QStringLiteral("G-%1").arg(secondId);
        pairState.riskLevel = sourceObject.riskLevel;
        snapshot.pairRiskStates.append(std::move(pairState));
    };

    for (const RiskObjectData& sourceObject : frame.objects) {
        appendObject(sourceObject, lifecycleOpacity(sourceObject.globalId, true, 0, localTimeMsec));
    }

    for (auto iterator = retainedObjects_.cbegin(); iterator != retainedObjects_.cend(); ++iterator) {
        const qint64 lastSeenSourceTimestamp = lastSeenSourceTimes_.value(iterator.key());
        const qint64 missingAgeMsec = targetSourceTimestamp - lastSeenSourceTimestamp;
        if (missingAgeMsec < 0 || missingAgeMsec > config_.missingGraceMsec + config_.fadeOutMsec) {
            continue;
        }

        appendObject(iterator.value(), lifecycleOpacity(iterator.key(), false, missingAgeMsec, localTimeMsec));
    }

    QHash<QString, DigitalTwinRiskLevel> currentPairRiskLevels;
    currentPairRiskLevels.reserve(snapshot.pairRiskStates.size());
    for (const DigitalTwinPairRiskState& pairState : snapshot.pairRiskStates) {
        const QString pairKey = pairState.firstObjectId + QStringLiteral("|") + pairState.secondObjectId;
        currentPairRiskLevels.insert(pairKey, pairState.riskLevel);

        const DigitalTwinRiskLevel previousRiskLevel =
            previousPairRiskLevels_.value(pairKey, DigitalTwinRiskLevel::Normal);
        const bool dangerToWarning =
            previousRiskLevel == DigitalTwinRiskLevel::Danger && pairState.riskLevel == DigitalTwinRiskLevel::Warning;
        const bool riskLevelChanged = previousRiskLevel != pairState.riskLevel;
        const bool repeatDue = localTimeMsec >= nextPairPulseTimesMsec_.value(pairKey, 0);

        if (dangerToWarning) {
            nextPairPulseTimesMsec_.insert(pairKey, localTimeMsec + pulseRepeatMsec(pairState.riskLevel));
            continue;
        }

        if (!riskLevelChanged && !repeatDue) {
            continue;
        }

        const auto firstPosition = currentPositions.constFind(pairState.firstObjectId);
        const auto secondPosition = currentPositions.constFind(pairState.secondObjectId);
        if (firstPosition == currentPositions.cend() || secondPosition == currentPositions.cend()) {
            continue;
        }

        DigitalTwinRiskEvent riskEvent;
        riskEvent.firstObjectId = pairState.firstObjectId;
        riskEvent.secondObjectId = pairState.secondObjectId;
        riskEvent.position = (*firstPosition + *secondPosition) * 0.5;
        riskEvent.riskLevel = pairState.riskLevel;
        pendingRiskEvents_.append(std::move(riskEvent));
        nextPairPulseTimesMsec_.insert(pairKey, localTimeMsec + pulseRepeatMsec(pairState.riskLevel));
    }

    for (auto iterator = nextPairPulseTimesMsec_.begin(); iterator != nextPairPulseTimesMsec_.end();) {
        if (currentPairRiskLevels.contains(iterator.key())) {
            ++iterator;
        } else {
            iterator = nextPairPulseTimesMsec_.erase(iterator);
        }
    }
    previousPairRiskLevels_ = std::move(currentPairRiskLevels);
    removeInactivePositionStates(currentPositions);
    previousPositions_ = std::move(currentPositions);
    return snapshot;
}

/**
 * @brief   마지막 스냅샷 계산에서 생성된 위험 펄스 이벤트를 반환합니다.
 * @return  UI 오버레이에 한 번씩 전달할 위험 이벤트 목록
 */
QVector<DigitalTwinRiskEvent> RiskObjectTracker::takeRiskEvents() {
    QVector<DigitalTwinRiskEvent> events = std::move(pendingRiskEvents_);
    pendingRiskEvents_.clear();
    return events;
}

/**
 * @brief                  수신 시각 전후 프레임 사이의 객체 위치를 보간합니다.
 * @param sourceTimestamp  표시 대상 원본 시각
 * @return                 보간된 통합 위험 프레임
 */
RiskFrameData RiskObjectTracker::interpolatedFrame(qint64 sourceTimestamp) const {
    const auto after = std::lower_bound(
        history_.cbegin(), history_.cend(), sourceTimestamp,
        [](const RiskFrameData& frame, qint64 timestamp) { return frame.sourceTimestamp < timestamp; });
    if (after == history_.cbegin()) {
        return history_.constFirst();
    }
    if (after == history_.cend()) {
        return history_.constLast();
    }
    if (after->sourceTimestamp == sourceTimestamp) {
        return *after;
    }

    const RiskFrameData& nextFrame = *after;
    const RiskFrameData& previousFrame = *std::prev(after);
    const qint64 durationMsec = nextFrame.sourceTimestamp - previousFrame.sourceTimestamp;
    if (durationMsec <= 0) {
        return nextFrame;
    }

    const double ratio = std::clamp(
        static_cast<double>(sourceTimestamp - previousFrame.sourceTimestamp) / static_cast<double>(durationMsec), 0.0,
        1.0);
    RiskFrameData result = previousFrame;
    result.sourceTimestamp = sourceTimestamp;

    QHash<qint64, const RiskObjectData*> previousObjects;
    QHash<qint64, const RiskObjectData*> nextObjects;
    previousObjects.reserve(previousFrame.objects.size());
    nextObjects.reserve(nextFrame.objects.size());
    for (const RiskObjectData& object : previousFrame.objects) {
        previousObjects.insert(object.globalId, &object);
    }
    for (const RiskObjectData& object : nextFrame.objects) {
        nextObjects.insert(object.globalId, &object);
    }

    for (RiskObjectData& object : result.objects) {
        const auto previousObject = previousObjects.constFind(object.globalId);
        const auto nextObject = nextObjects.constFind(object.globalId);
        if (previousObject != previousObjects.cend() && nextObject != nextObjects.cend() &&
            (*previousObject)->objectClass == (*nextObject)->objectClass) {
            object.worldPosition =
                interpolatePosition((*previousObject)->worldPosition, (*nextObject)->worldPosition, ratio);
        }
    }
    return result;
}

/** @brief 설정 좌표가 없을 때 관측 좌표로 지도 정규화 범위를 갱신합니다. */
void RiskObjectTracker::updateAutomaticWorldBounds(const RiskFrameData& frame) {
    if (hasConfiguredWorldBounds_ || frame.objects.isEmpty()) {
        return;
    }

    double minX = frame.objects.constFirst().worldPosition.x();
    double maxX = minX;
    double minY = frame.objects.constFirst().worldPosition.y();
    double maxY = minY;
    bool normalizedCoordinates = true;
    for (const RiskObjectData& object : frame.objects) {
        minX = qMin(minX, object.worldPosition.x());
        maxX = qMax(maxX, object.worldPosition.x());
        minY = qMin(minY, object.worldPosition.y());
        maxY = qMax(maxY, object.worldPosition.y());
        normalizedCoordinates = normalizedCoordinates && object.worldPosition.x() >= 0.0 &&
                                object.worldPosition.x() <= 1.0 && object.worldPosition.y() >= 0.0 &&
                                object.worldPosition.y() <= 1.0;
    }

    QRectF observedBounds;
    if (normalizedCoordinates) {
        observedBounds = QRectF(0.0, 0.0, 1.0, 1.0);
    } else {
        const double width = qMax(1.0, maxX - minX);
        const double height = qMax(1.0, maxY - minY);
        const double centerX = (minX + maxX) * 0.5;
        const double centerY = (minY + maxY) * 0.5;
        const double horizontalMargin = width * 0.08;
        const double verticalMargin = height * 0.08;
        observedBounds = QRectF(centerX - width * 0.5 - horizontalMargin, centerY - height * 0.5 - verticalMargin,
                                width + horizontalMargin * 2.0, height + verticalMargin * 2.0);
    }

    if (!hasAutomaticWorldBounds_) {
        automaticWorldBounds_ = observedBounds;
        hasAutomaticWorldBounds_ = true;
    } else {
        automaticWorldBounds_ = automaticWorldBounds_.united(observedBounds);
    }
}

/** @brief 월드 좌표를 지도에서 사용하는 0.0~1.0 좌표로 변환합니다. */
QPointF RiskObjectTracker::normalizedWorldPosition(const QPointF& worldPosition) const {
    const QRectF bounds = hasConfiguredWorldBounds_ ? configuredWorldBounds_ : automaticWorldBounds_;
    if (bounds.width() <= 0.0 || bounds.height() <= 0.0) {
        return QPointF(0.5, 0.5);
    }

    const double normalizedX = qBound(0.0, (worldPosition.x() - bounds.left()) / bounds.width(), 1.0);
    const double sourceY = qBound(0.0, (worldPosition.y() - bounds.top()) / bounds.height(), 1.0);
    const double normalizedY = invertWorldY_ ? 1.0 - sourceY : sourceY;
    return QPointF(normalizedX, normalizedY);
}

/**
 * @brief                   입력 좌표의 순간적인 튐을 제한하고 작은 위치 흔들림을 완화합니다.
 * @param objectId          추적 객체 식별자
 * @param measuredPosition  현재 프레임에서 계산한 정규화 좌표
 * @param localTimeMsec     현재 로컬 시각
 * @return                  화면에 사용할 안정화된 정규화 좌표
 */
QPointF RiskObjectTracker::stabilizedPosition(const QString& objectId, const QPointF& measuredPosition,
                                              qint64 localTimeMsec) {
    const auto positionIterator = stabilizedPositions_.constFind(objectId);
    const qint64 previousTimeMsec = stabilizedPositionTimesMsec_.value(objectId, 0);
    if (positionIterator == stabilizedPositions_.cend() || previousTimeMsec <= 0 || localTimeMsec <= previousTimeMsec ||
        localTimeMsec - previousTimeMsec > positionFilterResetGapMsec) {
        stabilizedPositions_.insert(objectId, measuredPosition);
        stabilizedPositionTimesMsec_.insert(objectId, localTimeMsec);
        return measuredPosition;
    }

    const QPointF previousPosition = *positionIterator;
    QPointF displacement = measuredPosition - previousPosition;
    const double distance = std::hypot(displacement.x(), displacement.y());
    const qint64 elapsedMsec =
        qBound(minimumPositionFilterStepMsec, localTimeMsec - previousTimeMsec, maximumPositionFilterStepMsec);
    const double maximumDistance = maximumNormalizedSpeedPerSecond * static_cast<double>(elapsedMsec) / 1000.0;

    if (distance > maximumDistance && distance > 0.0) {
        displacement *= maximumDistance / distance;
    }

    const QPointF stabilized(qBound(0.0, previousPosition.x() + displacement.x(), 1.0),
                             qBound(0.0, previousPosition.y() + displacement.y(), 1.0));
    stabilizedPositions_.insert(objectId, stabilized);
    stabilizedPositionTimesMsec_.insert(objectId, localTimeMsec);
    return stabilized;
}

/**
 * @brief                  gid별 표시 투명도를 단조롭게 갱신해 재등장 시에도 같은 item을 부드럽게 복구합니다.
 * @param objectId         통합 객체 ID
 * @param present          현재 보간 프레임에 객체가 있으면 true
 * @param missingAgeMsec   마지막 source frame 이후 누락 시간
 * @param localTimeMsec    현재 로컬 렌더 시각
 * @return                 0.0~1.0 범위의 표시 투명도
 */
qreal RiskObjectTracker::lifecycleOpacity(qint64 objectId, bool present, qint64 missingAgeMsec, qint64 localTimeMsec) {
    const bool knownObject = renderedOpacities_.contains(objectId);
    qreal opacity = renderedOpacities_.value(objectId, 0.0);
    const qint64 previousUpdateMsec = opacityUpdateTimesMsec_.value(objectId, localTimeMsec);
    const qint64 elapsedMsec = qBound<qint64>(0LL, localTimeMsec - previousUpdateMsec, 100LL);

    if (present) {
        opacity = config_.fadeInMsec <= 0
                      ? 1.0
                      : qMin(1.0, opacity + static_cast<qreal>(knownObject ? elapsedMsec : 0) / config_.fadeInMsec);
    } else if (missingAgeMsec > config_.missingGraceMsec) {
        opacity =
            config_.fadeOutMsec <= 0 ? 0.0 : qMax(0.0, opacity - static_cast<qreal>(elapsedMsec) / config_.fadeOutMsec);
    }

    renderedOpacities_.insert(objectId, opacity);
    opacityUpdateTimesMsec_.insert(objectId, localTimeMsec);
    return opacity;
}

/**
 * @brief                   현재 스냅샷에서 사라진 객체의 위치 보정 상태를 정리합니다.
 * @param currentPositions  현재 화면에 표시 중인 객체 위치
 */
void RiskObjectTracker::removeInactivePositionStates(const QHash<QString, QPointF>& currentPositions) {
    for (auto iterator = stabilizedPositions_.begin(); iterator != stabilizedPositions_.end();) {
        if (currentPositions.contains(iterator.key())) {
            ++iterator;
            continue;
        }

        stabilizedPositionTimesMsec_.remove(iterator.key());
        bool objectIdOk = false;
        const qint64 objectId = iterator.key().mid(2).toLongLong(&objectIdOk);
        if (objectIdOk) {
            renderedOpacities_.remove(objectId);
            opacityUpdateTimesMsec_.remove(objectId);
        }
        iterator = stabilizedPositions_.erase(iterator);
    }
}
