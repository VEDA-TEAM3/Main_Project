#include "model/RiskObjectTracker.h"

#include <QHash>
#include <QProcessEnvironment>
#include <QSet>
#include <algorithm>
#include <iterator>
#include <utility>

namespace {
constexpr qint64 riskSyncOffsetMsec = 100;
constexpr qint64 sourceRestartGapMsec = 5000;
constexpr qint64 sourceTimestampRollbackResetMsec = 1000;
constexpr qint64 objectRetentionMsec = 350;
constexpr qint64 warningPulseRepeatMsec = 1200;
constexpr qint64 dangerPulseRepeatMsec = 1500;
constexpr qsizetype maximumHistorySize = 16;

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
RiskObjectTracker::RiskObjectTracker() {
    hasConfiguredWorldBounds_ = readConfiguredWorldBounds(configuredWorldBounds_);
    invertWorldY_ = readInvertWorldY();
}

/** @brief 수신 이력과 객체 이동 이력을 초기화합니다. */
void RiskObjectTracker::reset() {
    history_.clear();
    retainedObjects_.clear();
    lastSeenSourceTimes_.clear();
    previousPositions_.clear();
    previousPairRiskLevels_.clear();
    nextPairPulseTimesMsec_.clear();
    pendingRiskEvents_.clear();
    automaticWorldBounds_ = {};
    lastArrivalTimeMsec_ = 0;
    sourceClockOffsetMsec_ = 0;
    lastRenderSourceTimestamp_ = 0;
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
    if (history_.isEmpty()) {
        sourceClockOffsetMsec_ = measuredClockOffsetMsec;
    } else {
        sourceClockOffsetMsec_ = (sourceClockOffsetMsec_ * 7 + measuredClockOffsetMsec) / 8;
    }

    for (const RiskObjectData& object : frame.objects) {
        retainedObjects_.insert(object.globalId, object);
        lastSeenSourceTimes_.insert(object.globalId, frame.sourceTimestamp);
    }

    for (auto iterator = lastSeenSourceTimes_.begin(); iterator != lastSeenSourceTimes_.end();) {
        if (frame.sourceTimestamp - iterator.value() <= objectRetentionMsec) {
            ++iterator;
            continue;
        }

        retainedObjects_.remove(iterator.key());
        iterator = lastSeenSourceTimes_.erase(iterator);
    }

    history_.append(std::move(frame));
    while (history_.size() > maximumHistorySize) {
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
DigitalTwinSnapshot RiskObjectTracker::buildSnapshot(qint64 localTimeMsec) {
    pendingRiskEvents_.clear();
    if (history_.isEmpty()) {
        return {};
    }

    updateAutomaticWorldBounds(history_.constLast());
    const qint64 calculatedSourceTimestamp = localTimeMsec - sourceClockOffsetMsec_ - riskSyncOffsetMsec;
    const qint64 latestSourceTimestamp = history_.constLast().sourceTimestamp;
    const qint64 targetSourceTimestamp =
        qMin(latestSourceTimestamp, qMax(calculatedSourceTimestamp, lastRenderSourceTimestamp_));
    lastRenderSourceTimestamp_ = targetSourceTimestamp;
    const RiskFrameData frame = interpolatedFrame(targetSourceTimestamp);

    DigitalTwinSnapshot snapshot;
    QHash<QString, QPointF> currentPositions;
    QSet<QString> pairKeys;
    QSet<qint64> includedObjectIds;
    snapshot.objects.reserve(qMax(frame.objects.size(), retainedObjects_.size()));

    const auto appendObject = [this, &snapshot, &currentPositions, &pairKeys,
                               &includedObjectIds](const RiskObjectData& sourceObject) {
        if (includedObjectIds.contains(sourceObject.globalId)) {
            return;
        }
        includedObjectIds.insert(sourceObject.globalId);

        DigitalTwinObject object;
        object.objectId = QStringLiteral("G-%1").arg(sourceObject.globalId);
        object.type = sourceObject.objectClass == QStringLiteral("Human") ? DigitalTwinObjectType::Pedestrian
                                                                          : DigitalTwinObjectType::Vehicle;
        object.position = normalizedWorldPosition(sourceObject.worldPosition);
        object.channelIndex = channelIndexForPosition(object.position);
        object.velocity = object.position - previousPositions_.value(object.objectId, object.position);
        object.riskLevel = sourceObject.riskLevel;
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
        appendObject(sourceObject);
    }

    for (auto iterator = retainedObjects_.cbegin(); iterator != retainedObjects_.cend(); ++iterator) {
        if (latestSourceTimestamp - lastSeenSourceTimes_.value(iterator.key()) <= objectRetentionMsec) {
            appendObject(iterator.value());
        }
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

    const RiskFrameData& nextFrame = *after;
    const RiskFrameData& previousFrame = *std::prev(after);
    const qint64 durationMsec = nextFrame.sourceTimestamp - previousFrame.sourceTimestamp;
    if (durationMsec <= 0) {
        return nextFrame;
    }

    const double ratio = std::clamp(
        static_cast<double>(sourceTimestamp - previousFrame.sourceTimestamp) / static_cast<double>(durationMsec), 0.0,
        1.0);
    const RiskFrameData& membershipFrame = ratio < 0.5 ? previousFrame : nextFrame;
    RiskFrameData result = membershipFrame;
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
