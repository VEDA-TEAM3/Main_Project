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
constexpr qint64 warningPulseRepeatMsec = 1200;
constexpr qint64 dangerPulseRepeatMsec = 1500;
// 같은 쌍의 위험도가 올라가도 이보다 자주는 울리지 않는다
constexpr qint64 minimumPairPulseIntervalMsec = 400;
// 서로 다른 쌍의 파동도 한 프레임에 몰리지 않도록 최소 간격을 둔다
constexpr qint64 minimumPulseSpacingMsec = 150;
// 쌍이 사라졌다 돌아와도 직전 상태를 기억하는 기간
constexpr qint64 pairPulseStateRetentionMsec = 5000;
constexpr qint64 positionFilterResetGapMsec = 500;
constexpr qint64 minimumPositionFilterStepMsec = 16;
// 허용 이동량은 실제 프레임 간격에 비례해야 한다. 이 상한을 리셋 기준보다 낮게 잡으면
// 배달이 늦은 구간에서 허용량만 고정되어(예: 300ms 만에 온 프레임에 100ms 분량만 허용)
// 정상 이동까지 깎인다. 리셋 기준을 넘어가면 어차피 무제한으로 받아들이므로 같은 값으로 둔다
constexpr qint64 maximumPositionFilterStepMsec = positionFilterResetGapMsec;
// 월드 좌표(m) 기준 상한. 정규화 좌표에 걸면 같은 상수가 지도 크기에 따라 전혀 다른 속도가 된다
// (60m 지도에서 0.9/s = 54m/s로 사실상 무방비, 15m 지도에서는 13.5m/s로 실제 차량을 깎아냄).
//
// 이상치가 화면에 남길 수 있는 최대 이탈 = 이 값 x 프레임 간격이다(100ms면 0.8m).
// 클라이언트 한 대가 담당하는 영역이 10x10m이므로 상한이 높을수록 이탈이 영역 대비 커진다.
// 반대로 실제 최고 속도보다 낮게 잡으면 정상 이동까지 깎여 객체가 계속 뒤처지므로,
// 현장 최고 속도의 1.5배 정도로 둔다: 8m/s = 29km/h (보행 1.4m/s, 구내 주행 3~5m/s 기준)
constexpr double maximumWorldSpeedMetersPerSecond = 8.0;
// 채널 경계를 이만큼 넘어서야 채널이 바뀐다. 담당 구역이 10x10m이고 채널이 그 4사분면이라
// 객체가 경계를 자주 넘는데, 표시 지연 때문에 경계 위에서 채널이 왕복하면 위험 테두리와
// 신고 대상 채널이 깜빡인다
constexpr double channelBoundaryHysteresisMeters = 0.3;
constexpr int automaticBoundsExpansionFrameCount = 3;
constexpr qint64 rateLimitLogIntervalMsec = 1000;
constexpr qsizetype worldPositionMedianSampleCount = 3;
// 중앙값 필터는 움직이는 객체의 좌표를 항상 한 프레임 분량만큼 되돌린다. 그 정상 동작까지
// '이상치 제거'로 세면 진단 로그가 매 프레임 남고 통계도 의미가 없어지므로, 이 크기를 넘는
// 보정만 실제 이상치를 걸러낸 것으로 본다
constexpr double notableFilterCorrectionMeters = 0.5;

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

/**
 * @brief                 좌표 진단 수준을 정합니다.
 * @param configuredLevel 설정 파일에서 온 수준
 * @return                0=끔, 1=1초 요약, 2=객체별 프레임 상세까지
 *
 * @details 현장에서 설정 파일을 고치지 않고 켤 수 있도록 VEDA_TOPVIEW_DEBUG가 설정을 덮어쓴다.
 */
int resolveTopViewDebugLevel(int configuredLevel) {
    const QString value =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("VEDA_TOPVIEW_DEBUG")).trimmed();
    if (value.isEmpty()) {
        return qBound(0, configuredLevel, 2);
    }

    bool numberOk = false;
    const int level = value.toInt(&numberOk);
    return numberOk ? qBound(0, level, 2) : qBound(0, configuredLevel, 2);
}

QString formatPoint(const QPointF& point) {
    return QStringLiteral("%1,%2").arg(point.x(), 0, 'f', 2).arg(point.y(), 0, 'f', 2);
}

/**
 * @brief        좌하단과 우하단 사분면에 붙는 채널 번호를 서로 맞바꿉니다.
 * @param index  0부터 시작하는 사분면 또는 채널 인덱스
 * @return       맞바꾼 인덱스
 *
 * @details 현장 CH03과 CH04는 사분면 순서와 반대로 설치되어 있다. 자기 자신이 역함수이므로
 *          채널 인덱스를 사분면으로 되돌릴 때도 같은 함수를 쓴다.
 */
int swapLowerChannels(int index) { return index == 2 ? 3 : (index == 3 ? 2 : index); }

int channelIndexForPosition(const QPointF& position) {
    const int column = position.x() >= 0.5 ? 1 : 0;
    const int row = position.y() >= 0.5 ? 1 : 0;
    return swapLowerChannels(row * 2 + column);
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
    } else {
        hasConfiguredWorldBounds_ = readConfiguredWorldBounds(configuredWorldBounds_);
        invertWorldY_ = hasConfiguredWorldBounds_ ? readInvertWorldY() : config_.world.invertY;
    }

    const int configuredLevel = config_.debugDetail ? 2 : (config_.debugLogging ? 1 : 0);
    diagnostics_.level = resolveTopViewDebugLevel(configuredLevel);
    diagnostics_.detailIntervalMsec = qMax(0, config_.debugDetailIntervalMsec);
    if (diagnostics_.level > 0) {
        qInfo().noquote() << QStringLiteral("[TV] on lvl=%1 inv=%2 cap=%3m/s")
                                 .arg(diagnostics_.level)
                                 .arg(invertWorldY_ ? 1 : 0)
                                 .arg(maximumWorldSpeedMetersPerSecond, 0, 'f', 1);
        qInfo().noquote() << QStringLiteral("[TV] %1").arg(worldBoundsDescription());
    }
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
    pairPulseStates_.clear();
    pendingRiskEvents_.clear();
    worldPositionHistories_.clear();
    channelIndexes_.clear();
    pendingExpansionBounds_ = {};
    pendingExpansionFrameCount_ = 0;
    lastArrivalTimeMsec_ = 0;
    lastDiagnosticsMsec_ = 0;
    lastRateLimitLogMsec_ = 0;
    lastPulseEmitMsec_ = 0;
    diagnostics_.previousSourceTimestamp = 0;
    diagnostics_.previousRawPositions.clear();
    diagnostics_.lastObjectLogMsec.clear();

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

    if (diagnostics_.level > 0 && lastArrivalTimeMsec_ > 0) {
        diagnostics_.arrivalIntervalsMsec.append(arrivalTimeMsec - lastArrivalTimeMsec_);
    }

    if (lastArrivalTimeMsec_ > 0 && arrivalTimeMsec - lastArrivalTimeMsec_ > sourceRestartGapMsec) {
        if (diagnostics_.level > 0) {
            qInfo().noquote() << QStringLiteral("[TV] restart gap=%1ms").arg(arrivalTimeMsec - lastArrivalTimeMsec_);
        }
        reset();
    }
    lastArrivalTimeMsec_ = arrivalTimeMsec;

    // RiskFrame.ts는 단조 증가하지 않는다: control-server가 윈도우에 모인 채널 관측 중
    // '가장 오래된' ts를 프레임 ts로 싣는데(ConcatFuser), 채널별 CCTV 시계가 서로 다르므로
    // 어느 채널이 윈도우에 들어왔는지에 따라 ts가 뒤로 갈 수 있다. ts로 순서를 매기면 그동안의
    // 프레임이 통째로 버려져 객체가 멈췄다가 한 번에 튄다. 순서는 도착 순(QoS 1, 토픽 내 순서
    // 보장)으로 잡고, ts는 재전송된 같은 프레임을 걸러내는 데만 쓴다
    if (!history_.isEmpty() && frame.sourceTimestamp == history_.constLast().sourceTimestamp) {
        ++diagnostics_.duplicateCount;
        return false;
    }

    ++frameSequence_;

    QVector<QPointF> rawPositions;
    QVector<QPointF> medianPositions;
    if (diagnostics_.level >= 2) {
        rawPositions.reserve(frame.objects.size());
        medianPositions.reserve(frame.objects.size());
    }

    // 정규화와 경계 확장 이전에 프레임 단위로 걸러야 이상치가 배율까지 흔드는 것을 막는다.
    // 속도 상한도 렌더 틱이 아니라 여기서 건다: 좌표는 프레임마다만 바뀌므로, 렌더 틱마다
    // 걸면 한 프레임 분량의 이동이 한 틱에 몰려 정상 이동까지 상한에 걸린다
    for (RiskObjectData& object : frame.objects) {
        const QPointF rawPosition = object.worldPosition;
        object.worldPosition = medianFilteredWorldPosition(object.globalId, object.worldPosition);
        const QPointF medianPosition = object.worldPosition;
        object.worldPosition = rateLimitedWorldPosition(object.globalId, object.worldPosition, arrivalTimeMsec);

        if (diagnostics_.level > 0) {
            const QPointF medianDelta = medianPosition - rawPosition;
            if (std::hypot(medianDelta.x(), medianDelta.y()) > notableFilterCorrectionMeters) {
                ++diagnostics_.medianRejectedCount;
            }
        }
        if (diagnostics_.level >= 2) {
            rawPositions.append(rawPosition);
            medianPositions.append(medianPosition);
        }
    }

    updateAutomaticWorldBounds(frame);

    if (diagnostics_.level > 0) {
        logFrameDiagnostics(frame, rawPositions, medianPositions, arrivalTimeMsec);
    }

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

        // 좌표 이력은 여기서 지우지 않는다. 표시가 끊긴 직후가 상류 coast가 끝나는 시점이라
        // 이상치가 가장 나오기 쉬운데, 이력을 버리면 돌아온 첫 좌표가 중앙값 필터를 못 받는다.
        // 이력은 removeInactivePositionStates가 속도 상한 상태와 같은 기준으로 정리한다
        retainedObjects_.remove(iterator.key());
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

    if (diagnostics_.level > 0 && config_.diagnosticsIntervalMsec > 0 &&
        localTimeMsec - lastDiagnosticsMsec_ >= config_.diagnosticsIntervalMsec) {
        qInfo().noquote() << QStringLiteral("[TV] latest ts=%1 age=%2ms obj=%3")
                                 .arg(latestSourceTimestamp)
                                 .arg(qMax<qint64>(0, localTimeMsec - lastArrivalTimeMsec_))
                                 .arg(frame.objects.size());
        lastDiagnosticsMsec_ = localTimeMsec;
    }

    DigitalTwinSnapshot snapshot;
    QHash<QString, QPointF> currentPositions;
    QSet<QString> pairKeys;
    QSet<qint64> includedObjectIds;
    snapshot.objects.reserve(qMax(frame.objects.size(), retainedObjects_.size()));

    const auto appendObject = [this, localTimeMsec, &snapshot, &currentPositions, &pairKeys, &includedObjectIds](
                                  const RiskObjectData& sourceObject, qreal opacity, qint64 objectFrameSequence) {
        if (includedObjectIds.contains(sourceObject.globalId)) {
            return;
        }
        includedObjectIds.insert(sourceObject.globalId);

        DigitalTwinObject object;
        object.objectId = QStringLiteral("G-%1").arg(sourceObject.globalId);
        object.type = sourceObject.objectClass == QStringLiteral("Human") ? DigitalTwinObjectType::Pedestrian
                                                                          : DigitalTwinObjectType::Vehicle;
        const QPointF targetPosition = normalizedWorldPosition(sourceObject.worldPosition);
        object.position = transitionedPosition(object.objectId, targetPosition, objectFrameSequence, localTimeMsec);
        object.channelIndex = channelIndexForObject(sourceObject.globalId, object.position);
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
        appendObject(sourceObject, lifecycleOpacity(sourceObject.globalId, true, 0, localTimeMsec), frameSequence_);
    }

    for (auto iterator = retainedObjects_.cbegin(); iterator != retainedObjects_.cend(); ++iterator) {
        const qint64 lastSeenArrivalMsec = lastSeenArrivalTimesMsec_.value(iterator.key(), 0);
        const qint64 missingAgeMsec = lastSeenArrivalMsec > 0 ? localTimeMsec - lastSeenArrivalMsec : 0;
        if (missingAgeMsec < 0 || missingAgeMsec > config_.missingGraceMsec + config_.fadeOutMsec) {
            continue;
        }

        const QString objectId = QStringLiteral("G-%1").arg(iterator.key());
        const qint64 retainedFrameSequence = positionTransitions_.value(objectId).targetFrameSequence;
        appendObject(iterator.value(), lifecycleOpacity(iterator.key(), false, missingAgeMsec, localTimeMsec),
                     retainedFrameSequence > 0 ? retainedFrameSequence : frameSequence_);
    }

    for (const DigitalTwinPairRiskState& pairState : snapshot.pairRiskStates) {
        const QString pairKey = pairState.firstObjectId + QStringLiteral("|") + pairState.secondObjectId;
        PairPulseState& state = pairPulseStates_[pairKey];

        const bool escalated = static_cast<int>(pairState.riskLevel) > static_cast<int>(state.riskLevel);
        const bool dangerToWarning =
            state.riskLevel == DigitalTwinRiskLevel::Danger && pairState.riskLevel == DigitalTwinRiskLevel::Warning;
        const bool firstPulse = state.lastPulseMsec <= 0;
        const qint64 sinceLastPulseMsec = localTimeMsec - state.lastPulseMsec;
        state.riskLevel = pairState.riskLevel;
        state.lastSeenMsec = localTimeMsec;

        if (dangerToWarning) {
            state.lastPulseMsec = localTimeMsec;
            continue;
        }

        // 위험도가 올라갈 때도 최소 간격은 지킨다. 상류에서 쌍이 한 프레임씩 사라졌다 나타나면
        // '레벨이 바뀌었다'가 매 프레임 참이 되어 같은 쌍의 파동이 수십 개씩 겹친다
        const bool repeatDue = sinceLastPulseMsec >= pulseRepeatMsec(pairState.riskLevel);
        const bool escalationDue = escalated && sinceLastPulseMsec >= minimumPairPulseIntervalMsec;
        if (!firstPulse && !repeatDue && !escalationDue) {
            continue;
        }

        // 여러 쌍이 같은 프레임에 조건을 만족해도 한꺼번에 쏟아내지 않는다
        if (lastPulseEmitMsec_ > 0 && localTimeMsec - lastPulseEmitMsec_ < minimumPulseSpacingMsec) {
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
        state.lastPulseMsec = localTimeMsec;
        lastPulseEmitMsec_ = localTimeMsec;
    }

    // 쌍이 잠깐 사라졌다고 상태를 지우면 재등장 즉시 다시 울린다. 시간으로만 정리한다
    for (auto iterator = pairPulseStates_.begin(); iterator != pairPulseStates_.end();) {
        if (localTimeMsec - iterator.value().lastSeenMsec > pairPulseStateRetentionMsec) {
            iterator = pairPulseStates_.erase(iterator);
        } else {
            ++iterator;
        }
    }
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
    qInfo().noquote() << QStringLiteral("[TV] bounds %1 x=%2..%3 y=%4..%5")
                             .arg(reason)
                             .arg(automaticWorldBounds_.left(), 0, 'f', 1)
                             .arg(automaticWorldBounds_.right(), 0, 'f', 1)
                             .arg(automaticWorldBounds_.top(), 0, 'f', 1)
                             .arg(automaticWorldBounds_.bottom(), 0, 'f', 1);
}

bool RiskObjectTracker::worldBoundsReady() const { return hasConfiguredWorldBounds_ || hasAutomaticWorldBounds_; }

/**
 * @brief                     경계에서 채널이 왕복하지 않도록 이력을 반영해 채널을 정합니다.
 * @param globalId            융합 객체 ID
 * @param normalizedPosition  현재 표시 중인 0.0~1.0 좌표
 * @return                    0부터 시작하는 채널 인덱스
 *
 * @details 채널은 담당 구역의 4사분면이므로 경계는 정규화 0.5의 두 축이다. 경계를 막 넘은
 *          상태에서는 표시 지연과 좌표 흔들림만으로 판정이 뒤집히므로, 이전 채널에서 벗어나려면
 *          경계를 channelBoundaryHysteresisMeters만큼 확실히 지나야 한다.
 */
int RiskObjectTracker::channelIndexForObject(qint64 globalId, const QPointF& normalizedPosition) {
    const QRectF bounds = hasConfiguredWorldBounds_ ? configuredWorldBounds_ : automaticWorldBounds_;
    const double horizontalMargin =
        bounds.width() > 0.0 ? channelBoundaryHysteresisMeters / bounds.width() : 0.0;
    const double verticalMargin =
        bounds.height() > 0.0 ? channelBoundaryHysteresisMeters / bounds.height() : 0.0;

    const auto previousIterator = channelIndexes_.constFind(globalId);
    const int measuredIndex = channelIndexForPosition(normalizedPosition);
    if (previousIterator == channelIndexes_.cend()) {
        channelIndexes_.insert(globalId, measuredIndex);
        return measuredIndex;
    }

    const int previousQuadrant = swapLowerChannels(*previousIterator);
    const int previousColumn = previousQuadrant % 2;
    const int previousRow = previousQuadrant / 2;
    const int column = previousColumn == 0 ? (normalizedPosition.x() >= 0.5 + horizontalMargin ? 1 : 0)
                                           : (normalizedPosition.x() < 0.5 - horizontalMargin ? 0 : 1);
    const int row = previousRow == 0 ? (normalizedPosition.y() >= 0.5 + verticalMargin ? 1 : 0)
                                     : (normalizedPosition.y() < 0.5 - verticalMargin ? 0 : 1);

    const int channelIndex = swapLowerChannels(row * 2 + column);
    channelIndexes_.insert(globalId, channelIndex);
    return channelIndex;
}

/** @brief 현재 사용 중인 정규화 범위와 그 출처를 사람이 읽을 수 있는 문자열로 만듭니다. */
QString RiskObjectTracker::worldBoundsDescription() const {
    if (!worldBoundsReady()) {
        return QStringLiteral("pending(warmup)");
    }

    const QRectF bounds = hasConfiguredWorldBounds_ ? configuredWorldBounds_ : automaticWorldBounds_;
    return QStringLiteral("bounds %1 x=%2..%3 y=%4..%5")
        .arg(hasConfiguredWorldBounds_ ? QStringLiteral("fix") : QStringLiteral("auto"))
        .arg(bounds.left(), 0, 'f', 1)
        .arg(bounds.right(), 0, 'f', 1)
        .arg(bounds.top(), 0, 'f', 1)
        .arg(bounds.bottom(), 0, 'f', 1);
}

/**
 * @brief                   프레임 단위 좌표 진단을 남깁니다.
 * @param frame             필터를 통과한 최신 프레임
 * @param rawPositions      필터 이전 월드 좌표 (level 2에서만 채워짐)
 * @param medianPositions   중앙값 필터 직후 월드 좌표 (level 2에서만 채워짐)
 * @param arrivalTimeMsec   로컬 수신 시각
 */
void RiskObjectTracker::logFrameDiagnostics(const RiskFrameData& frame, const QVector<QPointF>& rawPositions,
                                            const QVector<QPointF>& medianPositions, qint64 arrivalTimeMsec) {
    ++diagnostics_.frameCount;

    if (diagnostics_.previousSourceTimestamp > 0) {
        const qint64 timestampDelta = frame.sourceTimestamp - diagnostics_.previousSourceTimestamp;
        if (timestampDelta < 0) {
            ++diagnostics_.backwardTimestampCount;
        }
        diagnostics_.minTimestampDeltaMsec = diagnostics_.frameCount == 1
                                                 ? timestampDelta
                                                 : qMin(diagnostics_.minTimestampDeltaMsec, timestampDelta);
        diagnostics_.maximumTimestampDeltaMsec = qMax(diagnostics_.maximumTimestampDeltaMsec, timestampDelta);
    }
    diagnostics_.previousSourceTimestamp = frame.sourceTimestamp;

    if (diagnostics_.level >= 2 && rawPositions.size() == frame.objects.size()) {
        for (qsizetype index = 0; index < frame.objects.size(); ++index) {
            const RiskObjectData& object = frame.objects.at(index);
            const QPointF rawPosition = rawPositions.at(index);
            const QPointF previousRaw = diagnostics_.previousRawPositions.value(object.globalId, rawPosition);
            const QPointF rawDelta = rawPosition - previousRaw;
            diagnostics_.previousRawPositions.insert(object.globalId, rawPosition);

            // 객체가 여럿이면 프레임마다 전부 남기는 것만으로 초당 수백 줄이 되어 정작 볼 줄이 묻힌다.
            // gid마다 주기적으로만 남기되, 필터가 실제로 개입한 프레임은 주기와 무관하게 남긴다
            const QPointF medianDelta = medianPositions.at(index) - rawPosition;
            const QPointF limitDelta = object.worldPosition - medianPositions.at(index);
            const bool filterIntervened = std::hypot(medianDelta.x(), medianDelta.y()) > notableFilterCorrectionMeters ||
                                          std::hypot(limitDelta.x(), limitDelta.y()) > 0.001;
            // 이상치 하나를 따라잡는 동안 상한이 여러 프레임 연속으로 걸리므로, 개입 로그도
            // 더 짧은 주기로만 남긴다. 그래야 이상치 발생 사실은 놓치지 않으면서 줄 수가 안 터진다
            const qint64 lastLogMsec = diagnostics_.lastObjectLogMsec.value(object.globalId, 0);
            const qint64 requiredIntervalMsec =
                filterIntervened ? qMin<qint64>(diagnostics_.detailIntervalMsec, 250) : diagnostics_.detailIntervalMsec;
            if (lastLogMsec > 0 && arrivalTimeMsec - lastLogMsec < requiredIntervalMsec) {
                continue;
            }
            diagnostics_.lastObjectLogMsec.insert(object.globalId, arrivalTimeMsec);

            // 한 줄이 길면 붙여넣기·수집 과정에서 잘린다. 80자 안쪽으로 유지한다
            const bool boundsReady = worldBoundsReady();
            const QPointF normalized = boundsReady ? normalizedWorldPosition(object.worldPosition) : QPointF();
            QString line = QStringLiteral("[TV] g%1 raw=%2 d=%3 ch=%4")
                               .arg(object.globalId)
                               .arg(formatPoint(rawPosition))
                               .arg(std::hypot(rawDelta.x(), rawDelta.y()), 0, 'f', 2)
                               .arg(boundsReady ? QString::number(channelIndexForPosition(normalized) + 1)
                                                : QStringLiteral("-"));
            if (filterIntervened) {
                line += QStringLiteral(" cut med=%1 lim=%2")
                            .arg(std::hypot(medianDelta.x(), medianDelta.y()), 0, 'f', 2)
                            .arg(std::hypot(limitDelta.x(), limitDelta.y()), 0, 'f', 2);
            }
            qInfo().noquote() << line;
        }
    }

    if (diagnostics_.windowStartMsec <= 0) {
        diagnostics_.windowStartMsec = arrivalTimeMsec;
        return;
    }
    if (arrivalTimeMsec - diagnostics_.windowStartMsec >= 1000) {
        logDiagnosticsSummary(arrivalTimeMsec, frame.objects.size());
    }
}

/**
 * @brief                  1초 구간의 수신 상태 요약을 남기고 카운터를 초기화합니다.
 * @param arrivalTimeMsec  현재 구간의 종료 시각
 * @param objectCount      이번 프레임의 객체 수
 */
void RiskObjectTracker::logDiagnosticsSummary(qint64 arrivalTimeMsec, qsizetype objectCount) {
    QVector<qint64>& intervals = diagnostics_.arrivalIntervalsMsec;
    qint64 minimumInterval = 0;
    qint64 medianInterval = 0;
    qint64 maximumInterval = 0;
    if (!intervals.isEmpty()) {
        std::sort(intervals.begin(), intervals.end());
        minimumInterval = intervals.constFirst();
        medianInterval = intervals.at(intervals.size() / 2);
        maximumInterval = intervals.constLast();
    }

    // 한 줄에 다 담으면 붙여넣기 과정에서 잘리므로 수신/필터 두 줄로 나눈다
    qInfo().noquote() << QStringLiteral("[TV] rx f=%1 dup=%2 bts=%3 arr=%4/%5/%6ms obj=%7")
                             .arg(diagnostics_.frameCount)
                             .arg(diagnostics_.duplicateCount)
                             .arg(diagnostics_.backwardTimestampCount)
                             .arg(minimumInterval)
                             .arg(medianInterval)
                             .arg(maximumInterval)
                             .arg(objectCount);
    qInfo().noquote() << QStringLiteral("[TV] flt lim=%1 med=%2 ts=%3..%4ms")
                             .arg(diagnostics_.rateLimitedCount)
                             .arg(diagnostics_.medianRejectedCount)
                             .arg(diagnostics_.minTimestampDeltaMsec)
                             .arg(diagnostics_.maximumTimestampDeltaMsec);

    diagnostics_.windowStartMsec = arrivalTimeMsec;
    diagnostics_.frameCount = 0;
    diagnostics_.duplicateCount = 0;
    diagnostics_.backwardTimestampCount = 0;
    diagnostics_.minTimestampDeltaMsec = 0;
    diagnostics_.maximumTimestampDeltaMsec = 0;
    diagnostics_.rateLimitedCount = 0;
    diagnostics_.medianRejectedCount = 0;
    intervals.clear();
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
 * @param worldPosition      RiskFrame이 실어 온 월드 좌표(m)
 * @param localTimeMsec      현재 로컬 monotonic 시각
 * @return                   속도 상한을 적용한 월드 좌표(m)
 *
 * @details 다채널 융합은 같은 객체를 다른 카메라 관측으로 대표시키면서 한 프레임짜리 순간
 *          이동을 만든다. 그대로 두면 객체가 지도 반대편까지 갔다가 다음 프레임에 돌아온다.
 *          이동량을 실제 이동 속도 한계로 자르면 그런 이상치는 화면에서 거의 사라지고,
 *          진짜 이동은 계속 같은 방향으로 들어오므로 몇 프레임 안에 따라잡는다.
 *
 *          정규화 이전의 월드 좌표에 적용한다. 정규화 좌표에 걸면 상한의 실제 의미가 지도
 *          범위에 묶여, 경계 설정을 바꾸는 것만으로 억제 강도가 조용히 달라진다.
 */
QPointF RiskObjectTracker::rateLimitedWorldPosition(qint64 globalId, const QPointF& worldPosition,
                                                    qint64 arrivalTimeMsec) {
    const auto positionIterator = filteredPositions_.constFind(globalId);
    const qint64 previousTimeMsec = filteredPositionTimesMsec_.value(globalId, 0);
    if (positionIterator == filteredPositions_.cend() || previousTimeMsec <= 0 || arrivalTimeMsec <= previousTimeMsec ||
        arrivalTimeMsec - previousTimeMsec > positionFilterResetGapMsec) {
        filteredPositions_.insert(globalId, worldPosition);
        filteredPositionTimesMsec_.insert(globalId, arrivalTimeMsec);
        return worldPosition;
    }

    const QPointF previousPosition = *positionIterator;
    QPointF displacement = worldPosition - previousPosition;
    const double distance = std::hypot(displacement.x(), displacement.y());
    const qint64 elapsedMsec =
        qBound(minimumPositionFilterStepMsec, arrivalTimeMsec - previousTimeMsec, maximumPositionFilterStepMsec);
    const double maximumDistance = maximumWorldSpeedMetersPerSecond * static_cast<double>(elapsedMsec) / 1000.0;
    if (distance > maximumDistance && distance > 0.0) {
        displacement *= maximumDistance / distance;
        ++diagnostics_.rateLimitedCount;
        logRateLimitedJump(globalId, distance, maximumDistance, arrivalTimeMsec);
    }

    const QPointF limitedPosition = previousPosition + displacement;
    filteredPositions_.insert(globalId, limitedPosition);
    filteredPositionTimesMsec_.insert(globalId, arrivalTimeMsec);
    return limitedPosition;
}

/**
 * @brief                   잘라낸 순간 이동을 진단 로그로 남깁니다.
 * @param objectId          추적 객체 식별자
 * @param distance          측정된 이동량(m)
 * @param maximumDistance   허용 이동량(m)
 * @param localTimeMsec     현재 로컬 monotonic 시각
 *
 * @details 이 로그가 계속 찍히면 화면이 아니라 상류 융합/캘리브레이션이 흔들리는 것이다.
 */
void RiskObjectTracker::logRateLimitedJump(qint64 globalId, double distance, double maximumDistance,
                                           qint64 localTimeMsec) {
    if (diagnostics_.level <= 0 || localTimeMsec - lastRateLimitLogMsec_ < rateLimitLogIntervalMsec) {
        return;
    }
    lastRateLimitLogMsec_ = localTimeMsec;

    qInfo().noquote() << QStringLiteral("[TV] g%1 jump=%2m cap=%3m")
                             .arg(globalId)
                             .arg(distance, 0, 'f', 2)
                             .arg(maximumDistance, 0, 'f', 2);
}

/**
 * @brief                   새 Risk 좌표를 현재 표시 위치에서 목표 위치까지 로컬 시간으로 전환합니다.
 * @param objectId          추적 객체 식별자
 * @param targetPosition    최신 RiskFrame에서 계산한 목표 정규화 좌표
 * @param frameSequence     목표 좌표가 속한 프레임의 수신 순번 (RiskFrame.ts는 단조 증가가 아니라 쓰지 않는다)
 * @param localTimeMsec     현재 로컬 monotonic 시각
 * @return                  현재 렌더 시점의 정규화 좌표
 */
QPointF RiskObjectTracker::transitionedPosition(const QString& objectId, const QPointF& targetPosition,
                                                 qint64 frameSequence, qint64 localTimeMsec) {
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
        state.targetFrameSequence = frameSequence;
        positionTransitions_.insert(objectId, state);
        return targetPosition;
    }

    PositionTransitionState& state = iterator.value();
    if (frameSequence > state.targetFrameSequence) {
        const QPointF renderedNow = currentPosition(state);
        state.startPosition = renderedNow;
        state.targetPosition = targetPosition;
        state.transitionStartMsec = localTimeMsec;
        state.targetFrameSequence = frameSequence;
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
        worldPositionHistories_.remove(iterator.key());
        channelIndexes_.remove(iterator.key());
        iterator = filteredPositionTimesMsec_.erase(iterator);
    }
}
