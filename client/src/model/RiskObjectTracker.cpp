#include "model/RiskObjectTracker.h"

#include <QDebug>
#include <QHash>
#include <QProcessEnvironment>
#include <QSet>
#include <algorithm>
#include <cmath>
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
constexpr int automaticBoundsExpansionFrameCount = 3;
constexpr qint64 rateLimitLogIntervalMsec = 1000;
constexpr qsizetype worldPositionMedianSampleCount = 3;

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

double percentile(QVector<double> values, double fraction) {
    if (values.isEmpty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());
    const double boundedFraction = qBound(0.0, fraction, 1.0);
    const qsizetype index = static_cast<qsizetype>(boundedFraction * static_cast<double>(values.size() - 1));
    return values[index];
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
    lastSeenArrivalTimesMsec_.clear();
    renderedOpacities_.clear();
    opacityUpdateTimesMsec_.clear();
    previousPositions_.clear();
    positionTransitions_.clear();
    filteredPositions_.clear();
    filteredPositionTimesMsec_.clear();
    previousPairRiskLevels_.clear();
    nextPairPulseTimesMsec_.clear();
    pendingRiskEvents_.clear();
    worldPositionHistories_.clear();
    pendingExpansionBounds_ = {};
    pendingExpansionFrameCount_ = 0;
    lastArrivalTimeMsec_ = 0;
    lastDiagnosticsMsec_ = 0;
    lastRateLimitLogMsec_ = 0;

    // 지도 정규화 범위는 현장의 성질이지 수신 세션의 성질이 아니다. 스트림이 잠깐 끊겼다는
    // 이유로 버리면 재연결마다 새 warmup 창으로 배율이 다시 잡혀 화면 전체가 튄다
    if (!hasAutomaticWorldBounds_) {
        automaticWorldSamples_.clear();
        automaticBoundsStartSourceTimestamp_ = 0;
    }
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

    // 정규화와 경계 확장 이전에 프레임 단위로 걸러야 이상치가 배율까지 흔드는 것을 막는다
    for (RiskObjectData& object : frame.objects) {
        object.worldPosition = medianFilteredWorldPosition(object.globalId, object.worldPosition);
    }

    updateAutomaticWorldBounds(frame);

    for (const RiskObjectData& object : frame.objects) {
        retainedObjects_.insert(object.globalId, object);
        lastSeenArrivalTimesMsec_.insert(object.globalId, arrivalTimeMsec);
    }

    const qint64 objectRetentionMsec = config_.missingGraceMsec + config_.fadeOutMsec;
    for (auto iterator = lastSeenArrivalTimesMsec_.begin(); iterator != lastSeenArrivalTimesMsec_.end();) {
        if (arrivalTimeMsec - iterator.value() <= objectRetentionMsec) {
            ++iterator;
            continue;
        }

        retainedObjects_.remove(iterator.key());
        worldPositionHistories_.remove(iterator.key());
        iterator = lastSeenArrivalTimesMsec_.erase(iterator);
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
 * @brief                최신 Risk 상태를 로컬 전환 시간으로 부드럽게 표시할 스냅샷을 만듭니다.
 * @param localTimeMsec  현재 로컬 시각
 * @return               UI 렌더링용 디지털 트윈 스냅샷
 */
DigitalTwinSnapshot RiskObjectTracker::buildSnapshot(qint64 localTimeMsec) {
    pendingRiskEvents_.clear();
    if (history_.isEmpty() || !worldBoundsReady()) {
        return {};
    }

    const RiskFrameData& frame = history_.constLast();
    const qint64 latestSourceTimestamp = frame.sourceTimestamp;

    if (config_.diagnosticsIntervalMsec > 0 &&
        localTimeMsec - lastDiagnosticsMsec_ >= config_.diagnosticsIntervalMsec) {
        qInfo().noquote() << QStringLiteral(
                                 "[TOPVIEW] mode=latest-risk latestRiskTs=%1 arrivalAge=%2ms transition=%3ms history=%4")
                                 .arg(latestSourceTimestamp)
                                 .arg(qMax<qint64>(0, localTimeMsec - lastArrivalTimeMsec_))
                                 .arg(config_.positionTransitionMsec)
                                 .arg(history_.size());
        lastDiagnosticsMsec_ = localTimeMsec;
    }

    DigitalTwinSnapshot snapshot;
    QHash<QString, QPointF> currentPositions;
    QSet<QString> pairKeys;
    QSet<qint64> includedObjectIds;
    snapshot.objects.reserve(qMax(frame.objects.size(), retainedObjects_.size()));

    const auto appendObject = [this, localTimeMsec, &snapshot, &currentPositions, &pairKeys, &includedObjectIds](
                                  const RiskObjectData& sourceObject, qreal opacity, qint64 objectSourceTimestamp) {
        if (includedObjectIds.contains(sourceObject.globalId)) {
            return;
        }
        includedObjectIds.insert(sourceObject.globalId);

        DigitalTwinObject object;
        object.objectId = QStringLiteral("G-%1").arg(sourceObject.globalId);
        object.type = sourceObject.objectClass == QStringLiteral("Human") ? DigitalTwinObjectType::Pedestrian
                                                                          : DigitalTwinObjectType::Vehicle;
        const QPointF measuredPosition = normalizedWorldPosition(sourceObject.worldPosition);
        const QPointF targetPosition = rateLimitedPosition(object.objectId, measuredPosition, localTimeMsec);
        object.position = transitionedPosition(object.objectId, targetPosition, objectSourceTimestamp, localTimeMsec);
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
        appendObject(sourceObject, lifecycleOpacity(sourceObject.globalId, true, 0, localTimeMsec),
                     latestSourceTimestamp);
    }

    for (auto iterator = retainedObjects_.cbegin(); iterator != retainedObjects_.cend(); ++iterator) {
        const qint64 lastSeenArrivalMsec = lastSeenArrivalTimesMsec_.value(iterator.key(), 0);
        const qint64 missingAgeMsec = lastSeenArrivalMsec > 0 ? localTimeMsec - lastSeenArrivalMsec : 0;
        if (missingAgeMsec < 0 || missingAgeMsec > config_.missingGraceMsec + config_.fadeOutMsec) {
            continue;
        }

        const QString objectId = QStringLiteral("G-%1").arg(iterator.key());
        const qint64 retainedSourceTimestamp = positionTransitions_.value(objectId).targetSourceTimestamp;
        appendObject(iterator.value(), lifecycleOpacity(iterator.key(), false, missingAgeMsec, localTimeMsec),
                     retainedSourceTimestamp > 0 ? retainedSourceTimestamp : latestSourceTimestamp);
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
    removeInactivePositionStates(currentPositions, localTimeMsec);
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

/** @brief 설정 좌표가 없을 때 관측 좌표로 지도 정규화 범위를 갱신합니다. */
void RiskObjectTracker::updateAutomaticWorldBounds(const RiskFrameData& frame) {
    if (hasConfiguredWorldBounds_ || frame.objects.isEmpty()) {
        return;
    }

    if (hasAutomaticWorldBounds_) {
        expandAutomaticWorldBounds(frame);
        return;
    }

    if (automaticBoundsStartSourceTimestamp_ <= 0) {
        automaticBoundsStartSourceTimestamp_ = frame.sourceTimestamp;
    }

    for (const RiskObjectData& object : frame.objects) {
        if (automaticWorldSamples_.size() >= config_.world.automaticBoundsMaximumSamples) {
            break;
        }
        automaticWorldSamples_.append(object.worldPosition);
    }

    const bool warmupComplete =
        frame.sourceTimestamp - automaticBoundsStartSourceTimestamp_ >= config_.world.automaticBoundsWarmupMsec;
    if (!warmupComplete || automaticWorldSamples_.size() < config_.world.automaticBoundsMinimumSamples) {
        return;
    }

    QVector<double> xValues;
    QVector<double> yValues;
    xValues.reserve(automaticWorldSamples_.size());
    yValues.reserve(automaticWorldSamples_.size());
    for (const QPointF& sample : automaticWorldSamples_) {
        xValues.append(sample.x());
        yValues.append(sample.y());
    }

    const double lowerFraction = config_.world.automaticBoundsOutlierFraction;
    const double upperFraction = 1.0 - lowerFraction;
    const double minX = percentile(xValues, lowerFraction);
    const double maxX = percentile(xValues, upperFraction);
    const double minY = percentile(yValues, lowerFraction);
    const double maxY = percentile(yValues, upperFraction);
    // 최소 폭/높이는 관측 중심을 기준으로 넓힌다. minX를 원점으로 쓰면 관측 범위가 최소치보다
    // 좁을 때(정지 객체 등) 데이터가 지도 왼쪽 위 구석으로 몰린다
    const double width = qMax(1.0, maxX - minX);
    const double height = qMax(1.0, maxY - minY);
    const double centerX = (minX + maxX) * 0.5;
    const double centerY = (minY + maxY) * 0.5;
    const double horizontalMargin = width * config_.world.automaticBoundsPaddingRatio;
    const double verticalMargin = height * config_.world.automaticBoundsPaddingRatio;

    automaticWorldBounds_ =
        QRectF(centerX - width * 0.5 - horizontalMargin, centerY - height * 0.5 - verticalMargin,
               width + horizontalMargin * 2.0, height + verticalMargin * 2.0);
    hasAutomaticWorldBounds_ = true;
    automaticWorldSamples_.clear();
    logAutomaticWorldBounds(QStringLiteral("estimated"));
}

/**
 * @brief        추정된 자동 경계 밖의 좌표가 오면 그 좌표를 포함하도록 경계를 넓힙니다.
 * @param frame  검증을 통과한 최신 RiskFrame
 *
 * @details warmup 구간에서 굳힌 경계는 관측 창이 짧을수록 실제 도면보다 좁다. 경계를 그대로
 *          두면 바깥 좌표가 0..1 정규화에서 잘려 객체가 지도 가장자리에 달라붙는다. 범위 밖
 *          좌표가 올 때만 단조 확장하므로, 도면을 한 번 덮은 뒤에는 배율이 더 변하지 않는다.
 */
void RiskObjectTracker::expandAutomaticWorldBounds(const RiskFrameData& frame) {
    double left = automaticWorldBounds_.left();
    double right = automaticWorldBounds_.right();
    double top = automaticWorldBounds_.top();
    double bottom = automaticWorldBounds_.bottom();
    for (const RiskObjectData& object : frame.objects) {
        left = qMin(left, object.worldPosition.x());
        right = qMax(right, object.worldPosition.x());
        top = qMin(top, object.worldPosition.y());
        bottom = qMax(bottom, object.worldPosition.y());
    }

    const QRectF candidateBounds(left, top, right - left, bottom - top);
    if (candidateBounds == automaticWorldBounds_) {
        pendingExpansionFrameCount_ = 0;
        return;
    }

    // 융합 오류로 한 프레임만 튄 좌표에 지도 배율을 내주면 화면의 모든 객체가 한꺼번에 밀린다.
    // 연속 프레임에서 계속 경계 밖일 때만 실제 이동으로 보고 넓힌다
    pendingExpansionBounds_ =
        pendingExpansionFrameCount_ > 0 ? pendingExpansionBounds_.united(candidateBounds) : candidateBounds;
    if (++pendingExpansionFrameCount_ < automaticBoundsExpansionFrameCount) {
        return;
    }

    // 새 좌표가 경계선 위에 걸치면 다음 프레임에서 다시 확장이 돌므로 여백까지 함께 넓힌다
    const double horizontalMargin = pendingExpansionBounds_.width() * config_.world.automaticBoundsPaddingRatio;
    const double verticalMargin = pendingExpansionBounds_.height() * config_.world.automaticBoundsPaddingRatio;
    automaticWorldBounds_ =
        pendingExpansionBounds_.adjusted(-horizontalMargin, -verticalMargin, horizontalMargin, verticalMargin);
    pendingExpansionBounds_ = {};
    pendingExpansionFrameCount_ = 0;
    logAutomaticWorldBounds(QStringLiteral("expanded"));
}

/**
 * @brief         현재 자동 경계를 진단 로그로 남깁니다.
 * @param reason  경계가 갱신된 이유
 */
void RiskObjectTracker::logAutomaticWorldBounds(const QString& reason) const {
    qInfo().noquote() << QStringLiteral("[TOPVIEW MAP] Automatic world bounds %1 x=%2..%3 y=%4..%5")
                             .arg(reason)
                             .arg(automaticWorldBounds_.left(), 0, 'f', 3)
                             .arg(automaticWorldBounds_.right(), 0, 'f', 3)
                             .arg(automaticWorldBounds_.top(), 0, 'f', 3)
                             .arg(automaticWorldBounds_.bottom(), 0, 'f', 3);
}

bool RiskObjectTracker::worldBoundsReady() const { return hasConfiguredWorldBounds_ || hasAutomaticWorldBounds_; }

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
 * @brief                  gid별 최근 월드 좌표의 중앙값을 돌려줍니다.
 * @param globalId         융합 객체 ID
 * @param worldPosition    이번 프레임의 월드 좌표
 * @return                 중앙값 필터를 통과한 월드 좌표
 *
 * @details 속도 상한은 이상치의 '속도'만 자를 뿐 몇 프레임에 걸쳐 끌려가는 것은 막지 못한다.
 *          한 프레임만 튄 좌표는 중앙값에서 아예 탈락하므로 화면에도, 자동 경계 확장에도
 *          반영되지 않는다.
 *
 * ponytail: 표본 3개짜리 축별 중앙값이라 2프레임 이상 지속되는 이상치는 통과한다(속도 상한이
 *           2차 방어선). 더 필요하면 표본 수를 늘리거나 속도까지 모델링하는 추정기로 올린다.
 */
QPointF RiskObjectTracker::medianFilteredWorldPosition(qint64 globalId, const QPointF& worldPosition) {
    QVector<QPointF>& history = worldPositionHistories_[globalId];
    history.append(worldPosition);
    while (history.size() > worldPositionMedianSampleCount) {
        history.removeFirst();
    }

    if (history.size() < worldPositionMedianSampleCount) {
        return worldPosition;
    }

    QVector<double> xValues;
    QVector<double> yValues;
    xValues.reserve(history.size());
    yValues.reserve(history.size());
    for (const QPointF& sample : history) {
        xValues.append(sample.x());
        yValues.append(sample.y());
    }

    return QPointF(percentile(xValues, 0.5), percentile(yValues, 0.5));
}

/**
 * @brief                    한 프레임 만에 도달할 수 없는 이동량을 잘라 목표 좌표를 만듭니다.
 * @param objectId           추적 객체 식별자
 * @param measuredPosition   RiskFrame에서 계산한 정규화 좌표
 * @param localTimeMsec      현재 로컬 monotonic 시각
 * @return                   속도 상한을 적용한 목표 정규화 좌표
 *
 * @details 다채널 융합은 같은 객체를 다른 카메라 관측으로 대표시키면서 한 프레임짜리 순간
 *          이동을 만든다. 그대로 두면 객체가 지도 반대편까지 갔다가 다음 프레임에 돌아온다.
 *          이동량을 실제 이동 속도 한계로 자르면 그런 이상치는 화면에서 거의 사라지고,
 *          진짜 이동은 계속 같은 방향으로 들어오므로 몇 프레임 안에 따라잡는다.
 */
QPointF RiskObjectTracker::rateLimitedPosition(const QString& objectId, const QPointF& measuredPosition,
                                               qint64 localTimeMsec) {
    const auto positionIterator = filteredPositions_.constFind(objectId);
    const qint64 previousTimeMsec = filteredPositionTimesMsec_.value(objectId, 0);
    if (positionIterator == filteredPositions_.cend() || previousTimeMsec <= 0 || localTimeMsec <= previousTimeMsec ||
        localTimeMsec - previousTimeMsec > positionFilterResetGapMsec) {
        filteredPositions_.insert(objectId, measuredPosition);
        filteredPositionTimesMsec_.insert(objectId, localTimeMsec);
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
        logRateLimitedJump(objectId, distance, maximumDistance, localTimeMsec);
    }

    const QPointF limitedPosition(qBound(0.0, previousPosition.x() + displacement.x(), 1.0),
                                  qBound(0.0, previousPosition.y() + displacement.y(), 1.0));
    filteredPositions_.insert(objectId, limitedPosition);
    filteredPositionTimesMsec_.insert(objectId, localTimeMsec);
    return limitedPosition;
}

/**
 * @brief                   잘라낸 순간 이동을 진단 로그로 남깁니다.
 * @param objectId          추적 객체 식별자
 * @param distance          측정된 이동량
 * @param maximumDistance   허용 이동량
 * @param localTimeMsec     현재 로컬 monotonic 시각
 *
 * @details 이 로그가 계속 찍히면 화면이 아니라 상류 융합/캘리브레이션이 흔들리는 것이다.
 */
void RiskObjectTracker::logRateLimitedJump(const QString& objectId, double distance, double maximumDistance,
                                           qint64 localTimeMsec) {
    if (localTimeMsec - lastRateLimitLogMsec_ < rateLimitLogIntervalMsec) {
        return;
    }
    lastRateLimitLogMsec_ = localTimeMsec;

    qInfo().noquote() << QStringLiteral("[TOPVIEW] Rate-limited jump object=%1 measured=%2 allowed=%3")
                             .arg(objectId)
                             .arg(distance, 0, 'f', 3)
                             .arg(maximumDistance, 0, 'f', 3);
}

/**
 * @brief                   새 Risk 좌표를 현재 표시 위치에서 목표 위치까지 로컬 시간으로 전환합니다.
 * @param objectId          추적 객체 식별자
 * @param targetPosition    최신 RiskFrame에서 계산한 목표 정규화 좌표
 * @param sourceTimestamp   목표 좌표가 속한 RiskFrame source timestamp
 * @param localTimeMsec     현재 로컬 monotonic 시각
 * @return                  현재 렌더 시점의 정규화 좌표
 */
QPointF RiskObjectTracker::transitionedPosition(const QString& objectId, const QPointF& targetPosition,
                                                 qint64 sourceTimestamp, qint64 localTimeMsec) {
    auto currentPosition = [this, localTimeMsec](const PositionTransitionState& state) {
        if (config_.positionTransitionMsec <= 0 || state.transitionStartMsec <= 0) {
            return state.targetPosition;
        }

        const qint64 elapsedMsec = qMax<qint64>(0, localTimeMsec - state.transitionStartMsec);
        const double ratio = qBound(0.0, static_cast<double>(elapsedMsec) /
                                            static_cast<double>(config_.positionTransitionMsec),
                                    1.0);
        return interpolatePosition(state.startPosition, state.targetPosition, ratio);
    };

    auto iterator = positionTransitions_.find(objectId);
    if (iterator == positionTransitions_.end()) {
        PositionTransitionState state;
        state.startPosition = targetPosition;
        state.targetPosition = targetPosition;
        state.transitionStartMsec = localTimeMsec;
        state.targetSourceTimestamp = sourceTimestamp;
        positionTransitions_.insert(objectId, state);
        return targetPosition;
    }

    PositionTransitionState& state = iterator.value();
    if (sourceTimestamp > state.targetSourceTimestamp) {
        const QPointF renderedNow = currentPosition(state);
        state.startPosition = renderedNow;
        state.targetPosition = targetPosition;
        state.transitionStartMsec = localTimeMsec;
        state.targetSourceTimestamp = sourceTimestamp;
    }

    const QPointF rendered = currentPosition(state);
    return QPointF(qBound(0.0, rendered.x(), 1.0), qBound(0.0, rendered.y(), 1.0));
}

/**
 * @brief                  gid별 표시 투명도를 단조롭게 갱신해 재등장 시에도 같은 item을 부드럽게 복구합니다.
 * @param objectId         통합 객체 ID
 * @param present          현재 보간 프레임에 객체가 있으면 true
 * @param missingAgeMsec   마지막 로컬 수신 이후 누락 시간
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
 * @param localTimeMsec     현재 로컬 monotonic 시각
 */
void RiskObjectTracker::removeInactivePositionStates(const QHash<QString, QPointF>& currentPositions,
                                                     qint64 localTimeMsec) {
    for (auto iterator = positionTransitions_.begin(); iterator != positionTransitions_.end();) {
        if (currentPositions.contains(iterator.key())) {
            ++iterator;
            continue;
        }

        bool objectIdOk = false;
        const qint64 objectId = iterator.key().mid(2).toLongLong(&objectIdOk);
        if (objectIdOk) {
            renderedOpacities_.remove(objectId);
            opacityUpdateTimesMsec_.remove(objectId);
        }
        iterator = positionTransitions_.erase(iterator);
    }

    // 속도 상한 상태는 표시가 끊겨도 잠시 남긴다. 융합이 한두 프레임 객체를 놓쳤다가 되찾을 때
    // 상태를 이미 지웠으면 재등장 좌표를 그대로 받아들여 그 순간 튄다
    for (auto iterator = filteredPositionTimesMsec_.begin(); iterator != filteredPositionTimesMsec_.end();) {
        if (localTimeMsec - iterator.value() <= positionFilterResetGapMsec) {
            ++iterator;
            continue;
        }

        filteredPositions_.remove(iterator.key());
        iterator = filteredPositionTimesMsec_.erase(iterator);
    }
}
