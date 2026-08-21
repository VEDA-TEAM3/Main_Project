#include "ui/DigitalTwinMapWidget.h"

#include <QDebug>
#include <QMetaType>
#include <QQuickItem>
#include <QQuickWidget>
#include <QSet>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QVariant>
#include <QtGlobal>
#include <cmath>
#include <memory>
#include <utility>

#include "model/RiskObjectTracker.h"
#include "ui/DigitalTwinHeading.h"
#include "ui/DigitalTwinObjectStyleProvider.h"
#include "ui/DigitalTwinZoneIndex.h"
#include "ui/SharedQmlEngine.h"

namespace {
constexpr int maxTrailPointCount = 96;
constexpr double maximumStoredTrailPlanLength = 600.0;
/// QML로 넘기는 이동 경로 점의 상한. 보관 상한과 같게 두어 **이 값이 먼저 걸리지 않게** 한다.
/// 경로 길이는 설정값(movementTrailLength)이 정해야 하는데, 여기를 더 낮게 잡으면 수신이
/// 촘촘한 구간에서 점 개수가 먼저 차 버려서 설정과 무관하게 경로가 뭉텅 짧아진다
constexpr int maxPublishedTrailPointCount = maxTrailPointCount;
// 마커는 20Hz로 움직이되 Shape 경로는 10Hz로만 다시 만든다. 경로는 위치 판단에 쓰이지 않는
// 시각 효과라 이 주기로도 충분하고, QQuickWidget의 GUI 스레드 삼각분할 부하는 절반으로 줄어든다.
constexpr int trailPublishIntervalMsec = 100;
// 아이콘 방향에 쓸 이동 벡터의 지수이동평균 가중치. 이동량은 렌더 프레임 간 차분이라
// 저속에서 노이즈가 커서, 그대로 각도를 내면 아이콘이 제자리에서 떤다
constexpr double headingSmoothing = 0.25;
// 회전을 갱신할 최소 화면 이동량(도면 단위/프레임). 월드 단위로 잡으면 상류 캘리브레이션
// 배율이 바뀌는 것만으로 같은 상수가 전혀 다른 속도를 뜻하게 된다
constexpr double minimumHeadingPlanStep = 0.05;
// 이보다 작은 각도 변화는 눈에 띄지 않는다
constexpr double minimumHeadingChangeDegrees = 2.0;

QString centralEventKey(const CentralEventData& event) {
    const QString identity = event.eventId.isEmpty() ? event.eventType : event.eventId;
    return QStringLiteral("%1:%2").arg(event.channelIndex).arg(identity);
}

DigitalTwinRiskLevel riskLevelForSeverity(int severity) {
    if (severity >= 3) {
        return DigitalTwinRiskLevel::Danger;
    }
    if (severity > 0) {
        return DigitalTwinRiskLevel::Warning;
    }
    return DigitalTwinRiskLevel::Normal;
}

int riskPriority(DigitalTwinRiskLevel riskLevel) {
    switch (riskLevel) {
        case DigitalTwinRiskLevel::Danger:
            return 2;
        case DigitalTwinRiskLevel::Warning:
            return 1;
        case DigitalTwinRiskLevel::Normal:
            return 0;
    }

    return 0;
}

/**
 * @brief              두 도면 좌표 사이의 거리를 계산합니다.
 * @param firstPoint   첫 번째 좌표
 * @param secondPoint  두 번째 좌표
 */
double distanceBetween(const QPointF& firstPoint, const QPointF& secondPoint) {
    return std::hypot(firstPoint.x() - secondPoint.x(), firstPoint.y() - secondPoint.y());
}

/** @brief 이동 경로 polyline의 전체 길이를 계산합니다. */
double trailLength(const QVector<QPointF>& positions) {
    double length = 0.0;

    for (int i = 1; i < positions.size(); ++i) {
        length += distanceBetween(positions[i - 1], positions[i]);
    }

    return length;
}

/** @brief 이동 경로 점 개수와 전체 길이를 보관 범위로 줄입니다. */
void trimTrailPositions(QVector<QPointF>* positions, double maximumLength) {
    if (!positions) {
        return;
    }

    while (positions->size() > maxTrailPointCount) {
        positions->removeFirst();
    }

    while (positions->size() > 2 && trailLength(*positions) > maximumLength * 1.35) {
        positions->removeFirst();
    }
}

/**
 * @brief               설정 파일의 객체별 크기 비율을 유지하며 공통 배율을 적용합니다.
 * @param iconConfig    기본 차량·보행자 아이콘 크기
 * @param scalePercent  사용자 지정 백분율
 */
DigitalTwinIconConfig scaledIconConfig(const DigitalTwinIconConfig& iconConfig, int scalePercent) {
    DigitalTwinIconConfig scaledConfig = iconConfig;
    scaledConfig.vehiclePixels = qMax(8, qRound(iconConfig.vehiclePixels * scalePercent / 100.0));
    scaledConfig.pedestrianPixels = qMax(8, qRound(iconConfig.pedestrianPixels * scalePercent / 100.0));
    return scaledConfig;
}

/** @brief 스냅샷에 하나 이상의 위험 객체 또는 객체 쌍이 있는지 확인합니다. */
bool hasActiveDanger(const DigitalTwinSnapshot& snapshot) {
    for (const auto& pairRiskState : snapshot.pairRiskStates) {
        if (pairRiskState.riskLevel == DigitalTwinRiskLevel::Danger) {
            return true;
        }
    }

    for (const auto& object : snapshot.objects) {
        if (object.observed && object.riskLevel == DigitalTwinRiskLevel::Danger) {
            return true;
        }
    }

    return false;
}

/** @brief QML Image가 읽는 주소로 바꿉니다. 리소스 경로는 ":/..." 형태로 들어온다. */
QString qmlIconSource(const QString& resourcePath) {
    return resourcePath.startsWith(QLatin1Char(':')) ? QStringLiteral("qrc") + resourcePath : resourcePath;
}
}  // namespace

/**
 * @brief         QML 지도와 시뮬레이션 worker를 준비합니다.
 * @param parent  부모 위젯
 */
DigitalTwinMapWidget::DigitalTwinMapWidget(QWidget* parent)
    : QWidget(parent),
      objectStyleProvider_(std::make_shared<DefaultDigitalTwinObjectStyleProvider>()),
      riskObjectTracker_(std::make_unique<RiskObjectTracker>()) {
    qRegisterMetaType<DigitalTwinObject>("DigitalTwinObject");
    qRegisterMetaType<DigitalTwinRiskEvent>("DigitalTwinRiskEvent");
    qRegisterMetaType<DigitalTwinSnapshot>("DigitalTwinSnapshot");
    qRegisterMetaType<QVector<DigitalTwinObject>>("QVector<DigitalTwinObject>");

    liveClock_.start();
    liveFrameRenderTimer_.setInterval(liveConfig_.renderIntervalMsec);
    liveFrameRenderTimer_.setSingleShot(false);
    liveFrameRenderTimer_.setTimerType(Qt::CoarseTimer);
    connect(&liveFrameRenderTimer_, &QTimer::timeout, this, &DigitalTwinMapWidget::rebuildLiveSnapshot);

    setObjectName(QStringLiteral("digitalTwinMapWidget"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    mapView_ = createQmlPanelView(QStringLiteral("DigitalTwinMap.qml"), this);
    if (!mapView_) {
        qWarning() << "[DigitalTwinMapWidget] Failed to load DigitalTwinMap.qml";
        return;
    }

    layout->addWidget(mapView_);
    mapRoot_ = mapView_->rootObject();
    // QML 루트의 signal은 동적 metaobject에만 있으므로 문자열로 연결한다
    connect(mapRoot_, SIGNAL(zoneClicked(int)), this, SLOT(handleZoneClicked(int)));
}

/**
 * @brief        JSON에서 검증된 TopView 시간축과 월드 좌표 설정을 적용합니다.
 * @param config 실시간 렌더, 페이드, 동기화 및 고정 월드 좌표 설정
 *
 * @details 구역 수도 여기서 정해진다. 도면은 선언형이라 값을 바꾸면 QML이 알아서 다시 그린다 —
 *          예전 QGraphicsScene 구현처럼 재시작을 요구하지 않는다.
 */
void DigitalTwinMapWidget::configureLiveTracking(const DigitalTwinRuntimeConfig& config) {
    liveConfig_ = config;
    // 구역이 하나도 없으면 0이다. QML은 zoneCount 뒤쪽 칸을 "확장 예정"으로 그리므로,
    // 최초 배포 상태에서는 도면 전체가 빈 칸으로 뜬다. world.zoneCount()는 나눗셈에 쓰여
    // 최소 1을 돌려주므로 여기서는 실제 상자 수를 본다
    zoneCount_ = qBound(0, static_cast<int>(liveConfig_.world.zones.size()), digitalTwinMaximumZoneCount);
    ensureMapReady();
    setMapProperty("zoneCount", zoneCount_);
    refreshObjectAreas();

    liveFrameRenderTimer_.setInterval(liveConfig_.renderIntervalMsec);
    riskObjectTracker_ = std::make_unique<RiskObjectTracker>(liveConfig_);
    lastLiveSnapshotPublishMsec_ = 0;
    lastTrailPublishMsec_ = 0;
    publishedObjectPayload_.clear();
    publishedTrailPayload_.clear();
    hasPublishedObjectPayload_ = false;
    hasPublishedTrailPayload_ = false;

    deviceChannels_.resize(liveChannelCount());
    objectStyleProvider_ = std::make_shared<DefaultDigitalTwinObjectStyleProvider>(
        scaledIconConfig(liveConfig_.icons, displaySettings_.iconScalePercent));
    publishDeviceStates();
    publishObjects();
    publishTrails(true);
}

DigitalTwinMapWidget::~DigitalTwinMapWidget() = default;

/**
 * @brief          설정 팝업에서 확정한 맵 표시 옵션을 적용합니다.
 * @param settings 적용할 맵 표시 설정
 */
void DigitalTwinMapWidget::applyDisplaySettings(const DigitalTwinMapDisplaySettings& settings) {
    displaySettings_ = settings;
    objectStyleProvider_ = std::make_shared<DefaultDigitalTwinObjectStyleProvider>(
        scaledIconConfig(liveConfig_.icons, displaySettings_.iconScalePercent));
    publishDisplaySettings();
    publishObjects();
    publishTrails(true);
}

/**
 * @brief        MQTT 통합 RiskFrame을 실제 디지털 트윈 지도 입력으로 반영합니다.
 * @param frame  계약 검증을 통과한 4채널 통합 위험 프레임
 */
void DigitalTwinMapWidget::applyRiskFrame(RiskFrameData frame) {
    if (frame.sourceTimestamp <= 0) {
        qWarning() << "[DigitalTwinMapWidget] Invalid RiskFrame" << frame.sourceTimestamp;
        return;
    }

    if (!liveMode_) {
        qInfo() << "[DigitalTwinMapWidget] Live risk stream activated";
        liveMode_ = true;
        riskObjectTracker_->reset();
        lastLiveSnapshotPublishMsec_ = 0;
        // 새 실데이터 세션의 이동 경로 샘플 순서를 초기화한다.
        lastTrailSampleSequence_ = -1;
    }

    if (!riskObjectTracker_->submitFrame(std::move(frame), qMax<qint64>(1, liveClock_.elapsed()))) {
        return;
    }

    if (!liveFrameRenderTimer_.isActive()) {
        liveFrameRenderTimer_.start();
    }
}

void DigitalTwinMapWidget::applyCentralEvent(CentralEventData event) {
    if (event.channelIndex < 0 || event.channelIndex >= liveChannelCount() || event.sourceTimestamp <= 0) {
        return;
    }

    const QString key = centralEventKey(event);
    if (event.sourceTimestamp <= latestCentralEventSourceTimes_.value(key, 0)) {
        return;
    }
    latestCentralEventSourceTimes_.insert(key, event.sourceTimestamp);

    if (event.active) {
        activeCentralEvents_.insert(key, std::move(event));
    } else {
        activeCentralEvents_.remove(key);
    }

    if (liveMode_) {
        rebuildLiveSnapshot();
    } else {
        setDangerActive(hasActiveCentralDanger());
    }
}

/**
 * @brief           MQTT에서 확인된 채널별 장치 상태를 지도에 반영합니다.
 * @param statuses  이번 UI 주기에 변경된 장치 상태 목록
 */
void DigitalTwinMapWidget::applyDeviceChannelStatuses(QVector<DeviceChannelStatus> statuses) {
    bool changed = false;
    for (const DeviceChannelStatus& status : statuses) {
        if (status.channelIndex < 0 || status.channelIndex >= deviceChannels_.size()) {
            continue;
        }

        DeviceRecord& record = deviceChannels_[status.channelIndex];
        record.status = status;
        record.hasStatus = true;
        if (deviceSignalAvailable_ && status.hasConfirmedState &&
            status.feedbackHealth == DeviceFeedbackHealth::Confirmed) {
            record.receivedInCurrentSession = true;
        }
        changed = true;
    }

    if (changed) {
        publishDeviceStates();
    }
}

/**
 * @brief            MQTT 연결 여부에 따라 지도 장치 아이콘의 신호 상태를 변경합니다.
 * @param available  MQTT 브로커에 연결되어 있으면 true
 */
void DigitalTwinMapWidget::setDeviceSignalAvailable(bool available) {
    if (deviceSignalAvailable_ == available) {
        return;
    }

    deviceSignalAvailable_ = available;
    if (!available) {
        for (DeviceRecord& record : deviceChannels_) {
            record.receivedInCurrentSession = false;
        }
    }
    publishDeviceStates();
}

void DigitalTwinMapWidget::rebuildLiveSnapshot() {
    const qint64 currentTimeMsec = qMax<qint64>(1, liveClock_.elapsed());
    riskObjectTracker_->expireStaleFrame(currentTimeMsec, liveConfig_.frameExpiryMsec);
    const DigitalTwinSnapshot snapshot = riskObjectTracker_->buildSnapshot(currentTimeMsec);
    applyObjectUpdates(snapshot);
    publishChannelRiskLevels(snapshot);

    const bool shouldPublish =
        lastLiveSnapshotPublishMsec_ <= 0 ||
        currentTimeMsec - lastLiveSnapshotPublishMsec_ >= liveConfig_.snapshotPublishIntervalMsec ||
        !riskObjectTracker_->hasFrame();
    if (shouldPublish) {
        emit simulationSnapshotUpdated(snapshot);
        lastLiveSnapshotPublishMsec_ = currentTimeMsec;
    }

    setDangerActive(hasActiveCentralDanger() || hasActiveDanger(snapshot));
    if (!riskObjectTracker_->hasFrame()) {
        liveFrameRenderTimer_.stop();
    }
}

int DigitalTwinMapWidget::activeSeverityForChannel(int channelIndex) const {
    int severity = 0;
    for (auto iterator = activeCentralEvents_.cbegin(); iterator != activeCentralEvents_.cend(); ++iterator) {
        if (iterator.value().channelIndex == channelIndex) {
            severity = qMax(severity, iterator.value().severity);
        }
    }
    return severity;
}

bool DigitalTwinMapWidget::hasActiveCentralDanger() const {
    for (auto iterator = activeCentralEvents_.cbegin(); iterator != activeCentralEvents_.cend(); ++iterator) {
        if (iterator.value().severity >= 3) {
            return true;
        }
    }
    return false;
}

void DigitalTwinMapWidget::setDangerActive(bool active) {
    if (dangerActive_ == active) {
        return;
    }

    dangerActive_ = active;
    setMapProperty("dangerActive", active);
}

/**
 * @brief        설정 없이 화면에 올라간 경우에도 빈 지도가 남지 않도록 합니다.
 * @param event  Qt 표시 이벤트
 */
void DigitalTwinMapWidget::showEvent(QShowEvent* event) {
    ensureMapReady();
    QWidget::showEvent(event);
}

/**
 * @brief   구역 수가 정해진 뒤 빈 지도와 장치 상태를 한 번 준비합니다.
 */
void DigitalTwinMapWidget::ensureMapReady() {
    if (mapReady_) {
        return;
    }

    mapReady_ = true;
    setMapProperty("zoneCount", zoneCount_);
    refreshObjectAreas();
    deviceChannels_.resize(liveChannelCount());
    publishDisplaySettings();
    publishDeviceStates();
}

/**
 * @brief           채널별 위험 단계를 지도 부채꼴과 영상 테두리에 함께 반영합니다.
 * @param snapshot  현재 디지털 트윈 객체 상태
 *
 * @details 단계가 바뀐 프레임에만 알린다. 렌더 주기마다 같은 값을 다시 보내면 영상 테두리와
 *          패널이 매번 다시 그려진다.
 */
void DigitalTwinMapWidget::publishChannelRiskLevels(const DigitalTwinSnapshot& snapshot) {
    const QVector<DigitalTwinRiskLevel> riskLevels = channelRiskLevels(snapshot);
    if (riskLevels == publishedChannelRiskLevels_) {
        return;
    }
    publishedChannelRiskLevels_ = riskLevels;

    QVariantList levels;
    levels.reserve(riskLevels.size());
    for (const DigitalTwinRiskLevel riskLevel : riskLevels) {
        levels.append(riskPriority(riskLevel));
    }
    setMapProperty("channelRisk", levels);

    emit channelRiskLevelsChanged(riskLevels);
}

/**
 * @brief           객체와 중앙 이벤트를 합쳐 채널별 최고 위험 단계를 계산합니다.
 * @param snapshot  현재 디지털 트윈 객체 상태
 * @return          구역 수만큼의 채널 위험 단계 (CH-01부터 순서대로)
 */
QVector<DigitalTwinRiskLevel> DigitalTwinMapWidget::channelRiskLevels(const DigitalTwinSnapshot& snapshot) const {
    QVector<DigitalTwinRiskLevel> riskLevels(liveChannelCount(), DigitalTwinRiskLevel::Normal);

    for (const DigitalTwinObject& object : snapshot.objects) {
        if (!object.observed || object.channelIndex < 0 || object.channelIndex >= riskLevels.size()) {
            continue;
        }

        if (riskPriority(object.riskLevel) > riskPriority(riskLevels[object.channelIndex])) {
            riskLevels[object.channelIndex] = object.riskLevel;
        }
    }

    for (int channelIndex = 0; channelIndex < riskLevels.size(); ++channelIndex) {
        const DigitalTwinRiskLevel centralRisk = riskLevelForSeverity(activeSeverityForChannel(channelIndex));
        if (riskPriority(centralRisk) > riskPriority(riskLevels[channelIndex])) {
            riskLevels[channelIndex] = centralRisk;
        }
    }

    return riskLevels;
}

/**
 * @brief           worker가 계산한 객체 상태를 표시 상태에 반영합니다.
 * @param snapshot  최신 객체 상태 스냅샷
 */
void DigitalTwinMapWidget::applyObjectUpdates(const DigitalTwinSnapshot& snapshot) {
    const QVector<DigitalTwinObject>& objects = snapshot.objects;
    trailSampleFrame_ = snapshot.sampleSequence != lastTrailSampleSequence_;
    lastTrailSampleSequence_ = snapshot.sampleSequence;
    bool visualSetChanged = false;

    for (const auto& object : objects) {
        if (!visualIndexes_.contains(object.objectId)) {
            ObjectVisual visual;
            visual.object = object;
            visuals_.append(visual);
            visualIndexes_.insert(object.objectId, visuals_.size() - 1);
            updateObjectVisual(&visuals_.last());
            visualSetChanged = true;
            continue;
        }

        const qsizetype visualIndex = visualIndexes_.value(object.objectId);
        if (visualIndex < 0 || visualIndex >= visuals_.size()) {
            continue;
        }

        visuals_[visualIndex].object = object;
        updateObjectVisual(&visuals_[visualIndex]);
    }

    visualSetChanged = removeMissingVisuals(objects) || visualSetChanged;
    publishObjects();
    publishTrails(visualSetChanged);
}

/** @brief QML 구역 클릭을 받아 다시 알립니다. */
void DigitalTwinMapWidget::handleZoneClicked(int zoneIndex) {
    if (zoneIndex >= 0 && zoneIndex < zoneCount_) {
        emit zoneSelected(zoneIndex);
    }
}

/**
 * @brief          객체의 위치, 회전, 이동 경로를 갱신합니다.
 * @param visual   갱신할 표시 항목
 */
void DigitalTwinMapWidget::updateObjectVisual(ObjectVisual* visual) {
    if (!visual) {
        return;
    }

    const QPointF planPosition = planPointForObject(visual->object.position, visual->object.channelIndex);
    visual->planPosition = planPosition;

    // 방향은 월드 속도가 아니라 화면상 이동으로 정한다. invertY가 켜져 있으면 두 축의 부호가
    // 반대라, 월드 속도로 각도를 내면 화면에서 위로 가는 객체의 아이콘이 아래를 본다
    const QPointF planStep = visual->hasPreviousPlanPosition ? planPosition - visual->previousPlanPosition : QPointF();
    visual->previousPlanPosition = planPosition;
    visual->hasPreviousPlanPosition = true;
    visual->smoothedPlanVelocity =
        visual->smoothedPlanVelocity * (1.0 - headingSmoothing) + planStep * headingSmoothing;

    if (visual->object.type == DigitalTwinObjectType::Pedestrian) {
        visual->visibleRotationDegrees = 0.0;
    } else if (std::hypot(visual->smoothedPlanVelocity.x(), visual->smoothedPlanVelocity.y()) >=
               minimumHeadingPlanStep) {
        const double headingRotation =
            digitalTwinHeadingDegrees(visual->smoothedPlanVelocity) + digitalTwinHeadingOffsetDegrees;
        if (!visual->hasRotation ||
            std::fabs(digitalTwinAngleDifferenceDegrees(headingRotation, visual->visibleRotationDegrees)) >=
                minimumHeadingChangeDegrees) {
            visual->visibleRotationDegrees = headingRotation;
            visual->hasRotation = true;
        }
    }

    // 이동 경로에는 수신 샘플만 남긴다. 렌더 보간 프레임까지 쌓으면 96칸 버퍼가 보간 흔적으로
    // 차서 실제 궤적이 그만큼 짧아진다. 좌표가 그대로인 프레임까지 쌓으면(수신이 잠시 멈춘
    // 구간) 버퍼가 같은 점으로 채워져 실제 이동 경로가 밀려 나간다
    if (!trailSampleFrame_ || !visual->object.observed ||
        (!visual->recentPositions.isEmpty() && visual->recentPositions.constLast() == planPosition)) {
        return;
    }

    visual->recentPositions.append(planPosition);
    trimTrailPositions(&visual->recentPositions, maximumStoredTrailPlanLength);
}

/**
 * @brief   현재 객체 상태를 QML로 넘깁니다.
 *
 * @details 원소 하나가 평평한 배열이고 QML이 자리 순서로 읽는다. **QVariantMap을 담으면 안 된다** —
 *          그 목록이 소멸하는 자리에서 heap이 깨진다(CLAUDE.md의 DeviceStatusPanel 항목).
 *          안쪽 배열도 QVariant로 감싸야 한다. 감싸지 않으면 QList::append(const QList&)
 *          오버로드가 골라져 통째로 펼쳐진다.
 *
 *          자리: 0 id · 1 아이콘 · 2 x · 3 y · 4 크기 · 5 회전 · 6 투명도 ·
 *                7 이름표색 · 8 경로색. 이동 경로 좌표는 publishTrails()가 낮은 주기로 보냅니다.
 */
void DigitalTwinMapWidget::publishObjects() {
    QVariantList payload;
    payload.reserve(visuals_.size());

    for (const ObjectVisual& visual : visuals_) {
        const DigitalTwinObjectVisualStyle style = objectStyleProvider_->styleFor(visual.object);

        QVariantList fields;
        fields.append(visual.object.objectId);
        fields.append(qmlIconSource(style.iconPath));
        fields.append(visual.planPosition.x());
        fields.append(visual.planPosition.y());
        fields.append(static_cast<double>(style.iconSize.width()));
        fields.append(visual.visibleRotationDegrees);
        fields.append(static_cast<double>(visual.object.opacity));
        fields.append(style.labelColor.name());
        fields.append(style.trailColor.name());

        payload.append(QVariant(fields));
    }

    if (hasPublishedObjectPayload_ && payload == publishedObjectPayload_) {
        return;
    }

    publishedObjectPayload_ = payload;
    hasPublishedObjectPayload_ = true;
    setMapProperty("mapObjects", publishedObjectPayload_);
}

/**
 * @brief        이동 경로만 객체 위치보다 낮은 주기로 QML에 게시합니다.
 * @param force  객체 추가·삭제 또는 설정 변경으로 즉시 동기화해야 하는지 여부
 */
void DigitalTwinMapWidget::publishTrails(bool force) {
    const qint64 currentTimeMsec = qMax<qint64>(1, liveClock_.elapsed());
    if (!force && (!trailSampleFrame_ ||
                   (lastTrailPublishMsec_ > 0 && currentTimeMsec - lastTrailPublishMsec_ < trailPublishIntervalMsec))) {
        return;
    }

    QVariantList payload;
    payload.reserve(visuals_.size());
    for (const ObjectVisual& visual : visuals_) {
        QVariantList fields;
        fields.append(visual.object.objectId);

        if (displaySettings_.showMovementTrails) {
            const QVector<QPointF> trail = visibleTrail(visual.recentPositions);
            for (const QPointF& point : trail) {
                fields.append(point.x());
                fields.append(point.y());
            }
        }

        payload.append(QVariant(fields));
    }

    lastTrailPublishMsec_ = currentTimeMsec;
    if (hasPublishedTrailPayload_ && payload == publishedTrailPayload_) {
        return;
    }

    publishedTrailPayload_ = payload;
    hasPublishedTrailPayload_ = true;
    setMapProperty("mapTrails", publishedTrailPayload_);
}

/**
 * @brief   채널별 LED와 통합 알림 단계를 QML로 넘깁니다.
 *
 * @details 이번 세션에서 확인된 유효한 피드백이 없는 채널은 꺼짐으로 둔다. 브로커가 끊기면
 *          모든 채널이 한꺼번에 꺼짐으로 돌아가야 옛 상태가 살아 있는 것처럼 보이지 않는다.
 */
void DigitalTwinMapWidget::publishDeviceStates() {
    QVariantList ledStates;
    QVariantList alertStates;
    ledStates.reserve(deviceChannels_.size());
    alertStates.reserve(deviceChannels_.size());

    for (const DeviceRecord& record : deviceChannels_) {
        const bool valid = deviceSignalAvailable_ && record.hasStatus && record.receivedInCurrentSession &&
                           record.status.hasConfirmedState &&
                           record.status.feedbackHealth == DeviceFeedbackHealth::Confirmed;
        if (!valid) {
            ledStates.append(0);
            alertStates.append(0);
            continue;
        }

        const DeviceOutputState& outputs = record.status.outputs;
        int led = 0;
        if (outputs.ledRed) {
            led = 3;
        } else if (outputs.ledYellow) {
            led = 2;
        } else if (outputs.ledGreen) {
            led = 1;
        }
        ledStates.append(led);
        alertStates.append(outputs.beacon || outputs.buzzer ? 2 : 1);
    }

    setMapProperty("channelLed", ledStates);
    setMapProperty("channelAlert", alertStates);
}

void DigitalTwinMapWidget::publishDisplaySettings() {
    setMapProperty("showCctv", displaySettings_.showCctv);
    setMapProperty("showLed", displaySettings_.showLed);
    setMapProperty("showAlertDevice", displaySettings_.showAlertDevice);
    setMapProperty("showMovementTrails", displaySettings_.showMovementTrails);
}

void DigitalTwinMapWidget::setMapProperty(const char* name, const QVariant& value) {
    if (!mapRoot_) {
        return;
    }

    mapRoot_->setProperty(name, value);
}

/**
 * @brief          worker 목록에서 사라진 객체의 표시 항목을 제거합니다.
 * @param objects  현재 살아있는 객체 목록
 */
bool DigitalTwinMapWidget::removeMissingVisuals(const QVector<DigitalTwinObject>& objects) {
    QSet<QString> activeObjectIds;

    for (const auto& object : objects) {
        activeObjectIds.insert(object.objectId);
    }

    bool removedItem = false;

    for (qsizetype index = visuals_.size() - 1; index >= 0; --index) {
        if (!activeObjectIds.contains(visuals_[index].object.objectId)) {
            visuals_.removeAt(index);
            removedItem = true;
        }
    }

    if (removedItem) {
        rebuildVisualIndexes();
    }

    return removedItem;
}

/** @brief 표시 항목 제거 이후 객체 ID와 인덱스 매핑을 다시 구성합니다. */
void DigitalTwinMapWidget::rebuildVisualIndexes() {
    visualIndexes_.clear();

    for (qsizetype index = 0; index < visuals_.size(); ++index) {
        visualIndexes_.insert(visuals_[index].object.objectId, index);
    }
}

/**
 * @brief   구역별 객체 표시 영역을 QML에서 읽어 옵니다.
 *
 * @details 도면 기하의 원본은 ParkingPlan.js다. 여기서 같은 계산을 다시 하면 언젠가 한쪽만
 *          고쳐져 객체가 도면 밖에 찍힌다. 값은 [x,y,w,h]를 구역 수만큼 이어 붙인 평평한 배열이다.
 */
void DigitalTwinMapWidget::refreshObjectAreas() {
    objectAreaRects_.clear();
    if (!mapRoot_) {
        return;
    }

    const QVariantList areas = mapRoot_->property("objectAreas").toList();
    for (int index = 0; index + 3 < areas.size(); index += 4) {
        objectAreaRects_.append(QRectF(areas[index].toDouble(), areas[index + 1].toDouble(),
                                       areas[index + 2].toDouble(), areas[index + 3].toDouble()));
    }

    // 구역이 0개면 도면이 영역을 안 돌려주는 것이 정상이다
    if (objectAreaRects_.isEmpty() && zoneCount_ > 0) {
        qWarning() << "[DigitalTwinMapWidget] Plan returned no object areas; objects will be hidden";
    }
}

/** @brief 월드 좌표를 해당 물리 CCTV의 정사각형 도면 좌표로 변환합니다. */
QPointF DigitalTwinMapWidget::planPointForObject(const QPointF& worldPosition, int channelIndex) const {
    if (objectAreaRects_.isEmpty()) {
        return QPointF();
    }

    const QRectF worldBounds = liveConfig_.world.bounds;
    const bool validBounds = worldBounds.width() > 0.0 && worldBounds.height() > 0.0;
    if (!validBounds) {
        return QPointF();
    }

    // 어느 물리 CCTV 맵에 그릴지는 서버 zoneId가 정하고, 맵 안에서의 위치는 그 구역의
    // 월드 상자로 정규화한다
    const int zoneIndex = qBound(
        0, digitalTwinZoneIndex(channelIndex, zoneCount_, liveConfig_.world.zoneIndexForWorldX(worldPosition.x())),
        static_cast<int>(objectAreaRects_.size()) - 1);
    const QRectF zoneBounds = liveConfig_.world.zoneBounds(zoneIndex);
    const double normalizedX = qBound(0.0, (worldPosition.x() - zoneBounds.left()) / zoneBounds.width(), 1.0);
    double normalizedY = qBound(0.0, (worldPosition.y() - zoneBounds.top()) / zoneBounds.height(), 1.0);
    if (liveConfig_.world.invertY) {
        normalizedY = 1.0 - normalizedY;
    }

    const QRectF& area = objectAreaRects_[zoneIndex];
    return QPointF(area.left() + normalizedX * area.width(), area.top() + normalizedY * area.height());
}

/**
 * @brief           최근 위치 목록에서 설정한 길이만큼만 잘라 냅니다.
 * @param positions  도면 좌표계 기준 최근 위치 목록
 * @return          최신 쪽부터 movementTrailLength만큼의 점 목록
 */
QVector<QPointF> DigitalTwinMapWidget::visibleTrail(const QVector<QPointF>& positions) const {
    QVector<QPointF> visiblePositions;

    if (positions.isEmpty()) {
        return visiblePositions;
    }

    visiblePositions.prepend(positions.last());
    double accumulatedLength = 0.0;

    for (qsizetype index = positions.size() - 1; index > 0; --index) {
        if (visiblePositions.size() >= maxPublishedTrailPointCount) {
            break;
        }

        const QPointF currentPoint = positions[index];
        const QPointF previousPoint = positions[index - 1];
        const double segmentLength = distanceBetween(previousPoint, currentPoint);

        if (qFuzzyIsNull(segmentLength)) {
            continue;
        }

        if (accumulatedLength + segmentLength >= displaySettings_.movementTrailLength) {
            const double remainingLength = displaySettings_.movementTrailLength - accumulatedLength;
            const double ratio = remainingLength / segmentLength;
            visiblePositions.prepend(currentPoint + (previousPoint - currentPoint) * ratio);
            break;
        }

        visiblePositions.prepend(previousPoint);
        accumulatedLength += segmentLength;
    }

    return visiblePositions;
}
