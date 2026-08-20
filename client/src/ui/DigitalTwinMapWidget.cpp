#include "ui/DigitalTwinMapWidget.h"

#include <QDebug>
#include <QMetaObject>
#include <QMetaType>
#include <QQuickItem>
#include <QQuickWidget>
#include <QSet>
#include <QShowEvent>
#include <QThread>
#include <QVBoxLayout>
#include <QVariant>
#include <QtGlobal>
#include <cmath>
#include <memory>
#include <utility>

#include "model/DigitalTwinSimulationWorker.h"
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
    liveFrameExpiryTimer_.setInterval(liveConfig_.frameExpiryPollMsec);
    liveFrameExpiryTimer_.setTimerType(Qt::CoarseTimer);
    connect(&liveFrameExpiryTimer_, &QTimer::timeout, this, &DigitalTwinMapWidget::expireStaleLiveFrames);
    liveFrameRenderTimer_.setInterval(liveConfig_.renderIntervalMsec);
    liveFrameRenderTimer_.setSingleShot(false);
    liveFrameRenderTimer_.setTimerType(Qt::PreciseTimer);
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
    zoneCount_ = qBound(1, liveConfig_.world.zoneCount(), digitalTwinMaximumZoneCount);
    ensureMapReady();
    setMapProperty("zoneCount", zoneCount_);
    refreshObjectAreas();

    liveFrameExpiryTimer_.setInterval(liveConfig_.frameExpiryPollMsec);
    liveFrameRenderTimer_.setInterval(liveConfig_.renderIntervalMsec);
    riskObjectTracker_ = std::make_unique<RiskObjectTracker>(liveConfig_);
    lastLiveSnapshotPublishMsec_ = 0;

    deviceChannels_.resize(liveChannelCount());
    objectStyleProvider_ = std::make_shared<DefaultDigitalTwinObjectStyleProvider>(
        scaledIconConfig(liveConfig_.icons, displaySettings_.iconScalePercent));
    publishDeviceStates();
    publishObjects();
}

/** @brief worker 스레드를 안전하게 정리합니다. */
DigitalTwinMapWidget::~DigitalTwinMapWidget() {
    if (simulationWorker_) {
        disconnect(simulationWorker_.get(), nullptr, this, nullptr);
    }

    if (simulationWorker_ && simulationWorker_->thread() == &simulationThread_ && simulationThread_.isRunning()) {
        QThread* ownerThread = thread();
        QMetaObject::invokeMethod(
            simulationWorker_.get(),
            [worker = simulationWorker_.get(), ownerThread]() {
                worker->stop();
                worker->moveToThread(ownerThread);
            },
            Qt::BlockingQueuedConnection);
    }

    simulationThread_.quit();
    simulationThread_.wait();
    simulationWorker_.reset();
}

/** @brief 시뮬레이션 worker에 데모 시작을 요청합니다. */
void DigitalTwinMapWidget::startDemo() {
    if (!simulationWorker_ || !simulationThread_.isRunning()) {
        return;
    }

    QMetaObject::invokeMethod(simulationWorker_.get(), &DigitalTwinSimulationWorker::start, Qt::QueuedConnection);
}

/** @brief 시뮬레이션 worker의 주기 갱신을 중지합니다. */
void DigitalTwinMapWidget::stopDemo() {
    if (!simulationWorker_ || !simulationThread_.isRunning()) {
        return;
    }

    QMetaObject::invokeMethod(simulationWorker_.get(), &DigitalTwinSimulationWorker::stop,
                              Qt::BlockingQueuedConnection);
}

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
        stopDemo();
        liveMode_ = true;
        riskObjectTracker_->reset();
        lastLiveSnapshotPublishMsec_ = 0;
        // 데모와 실데이터는 서로 다른 카운터라 값이 겹칠 수 있다
        lastTrailSampleSequence_ = -1;
        liveFrameExpiryTimer_.start();
        emit liveRiskStreamActivated();
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

void DigitalTwinMapWidget::expireStaleLiveFrames() {
    const qint64 currentTimeMsec = qMax<qint64>(1, liveClock_.elapsed());
    const bool riskExpired = riskObjectTracker_->expireStaleFrame(currentTimeMsec, liveConfig_.frameExpiryMsec);
    if (riskExpired) {
        rebuildLiveSnapshot();
    }

    if (!riskObjectTracker_->hasFrame()) {
        liveFrameRenderTimer_.stop();
    }
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
 * @brief   구역 수가 정해진 뒤 worker와 데모를 한 번만 시작합니다.
 *
 * @details 도면 자체는 생성자에서 이미 QML로 올라가 있다. 여기서 하는 일은 구역 수에 맞춰
 *          객체 영역을 읽고 데모를 돌리는 것뿐이다.
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
    setupSimulationWorker();
    startDemo();
}

/** @brief 객체 이동과 위험 판정을 담당하는 worker를 별도 스레드에 연결합니다. */
void DigitalTwinMapWidget::setupSimulationWorker() {
    simulationWorker_ = std::make_shared<DigitalTwinSimulationWorker>();
    // 데모는 활성 구역을 가로로 이어 붙인 띠 위에서 객체를 돌린다. 스레드로 옮기기 전에
    // 넘겨야 경합이 없다. 데모 전용 값이라 실 데이터 경로에는 영향이 없다
    simulationWorker_->setZoneCount(zoneCount_);

    if (!simulationWorker_->moveToThread(&simulationThread_)) {
        qWarning() << "[DigitalTwinMapWidget] Failed to move simulation worker to its thread";
        simulationWorker_.reset();
        return;
    }

    connect(simulationWorker_.get(), &DigitalTwinSimulationWorker::snapshotUpdated, this,
            &DigitalTwinMapWidget::applySimulationSnapshot, Qt::QueuedConnection);

    simulationThread_.setObjectName(QStringLiteral("digital-twin-simulation"));
    simulationThread_.start();
}

/**
 * @brief           worker 스냅샷을 지도에 반영한 뒤 대시보드 소비자에게 전달합니다.
 * @param snapshot  객체와 객체 쌍 위험 상태를 함께 담은 최신 스냅샷
 */
void DigitalTwinMapWidget::applySimulationSnapshot(const DigitalTwinSnapshot& snapshot) {
    setDangerActive(hasActiveCentralDanger() || hasActiveDanger(snapshot));

    applyObjectUpdates(snapshot);
    publishChannelRiskLevels(snapshot);
    emit simulationSnapshotUpdated(snapshot);
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

    for (const auto& object : objects) {
        if (!visualIndexes_.contains(object.objectId)) {
            ObjectVisual visual;
            visual.object = object;
            visuals_.append(visual);
            visualIndexes_.insert(object.objectId, visuals_.size() - 1);
            updateObjectVisual(&visuals_.last());
            continue;
        }

        const qsizetype visualIndex = visualIndexes_.value(object.objectId);
        if (visualIndex < 0 || visualIndex >= visuals_.size()) {
            continue;
        }

        visuals_[visualIndex].object = object;
        updateObjectVisual(&visuals_[visualIndex]);
    }

    removeMissingVisuals(objects);
    publishObjects();
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
 *                7 이름표색 · 8 경로색 · 9.. 경로 좌표
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

        if (displaySettings_.showMovementTrails) {
            const QVector<QPointF> trail = visibleTrail(visual.recentPositions);
            for (const QPointF& point : trail) {
                fields.append(point.x());
                fields.append(point.y());
            }
        }

        payload.append(QVariant(fields));
    }

    setMapProperty("mapObjects", payload);
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
void DigitalTwinMapWidget::removeMissingVisuals(const QVector<DigitalTwinObject>& objects) {
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

    if (objectAreaRects_.isEmpty()) {
        qWarning() << "[DigitalTwinMapWidget] Plan returned no object areas; objects will be hidden";
    }
}

/**
 * @brief            데모 객체가 움직일 구역 사각형을 돌려줍니다.
 * @param zoneIndex  구역 번호
 * @return           객체 영역을 이웃과의 틈 절반만큼 좌우로 넓힌 사각형
 *
 * @details 구역별 객체 영역은 서로 떨어져 있습니다. 아이콘이 구역 테두리를 넘지 않도록
 *          도면이 남겨 둔 여백인데, **데모 객체는 구역을 건너다니므로** 이 틈을 그대로 두면
 *          경계에서 한 프레임에 틈 너비만큼 순간 이동합니다. 좌우로 틈의 절반씩 넓히면 이웃
 *          사각형과 정확히 맞닿아(왼쪽 끝 = 이웃의 오른쪽 끝) 끊김 없이 넘어갑니다.
 *
 *          양 끝 구역은 바깥쪽을 안쪽과 같은 폭으로 넓혀 진입·이탈 거리를 좌우 대칭으로
 *          맞춥니다. 구역이 줄바꿈되는 배치(4개 이상)에서는 이웃이 왼쪽으로 되돌아가므로
 *          틈이 음수가 되는데, 그때는 넓히지 않습니다.
 *
 *          **데모 전용입니다.** 실 데이터는 서버 zoneId와 구역 월드 상자로 자리를 정하므로
 *          이 함수를 타지 않습니다.
 */
QRectF DigitalTwinMapWidget::demoAreaForZone(int zoneIndex) const {
    const QRectF& area = objectAreaRects_[zoneIndex];
    double leftPad = 0.0;
    double rightPad = 0.0;

    if (zoneIndex > 0) {
        leftPad = qMax(0.0, area.left() - objectAreaRects_[zoneIndex - 1].right()) / 2.0;
    }

    if (zoneIndex + 1 < objectAreaRects_.size()) {
        rightPad = qMax(0.0, objectAreaRects_[zoneIndex + 1].left() - area.right()) / 2.0;
    }

    if (zoneIndex == 0) {
        leftPad = rightPad;
    }

    if (zoneIndex + 1 >= objectAreaRects_.size()) {
        rightPad = leftPad;
    }

    return area.adjusted(-leftPad, 0.0, rightPad, 0.0);
}

/** @brief 월드 좌표를 해당 물리 CCTV의 정사각형 도면 좌표로 변환합니다. */
QPointF DigitalTwinMapWidget::planPointForObject(const QPointF& worldPosition, int channelIndex) const {
    if (objectAreaRects_.isEmpty()) {
        return QPointF();
    }

    const QRectF worldBounds = liveConfig_.world.bounds;
    const bool validBounds = worldBounds.width() > 0.0 && worldBounds.height() > 0.0;
    if (!liveMode_ || !validBounds) {
        const int demoZoneIndex =
            qBound(0, channelIndex / digitalTwinChannelsPerZone, static_cast<int>(objectAreaRects_.size()) - 1);
        // 구역 사이 틈까지 덮는 사각형을 쓴다. 이웃과 맞닿아 있어야 구역을 넘는 순간
        // 객체가 튀지 않는다
        const QRectF demoArea = demoAreaForZone(demoZoneIndex);
        return QPointF(demoArea.left() + qBound(0.0, worldPosition.x(), 1.0) * demoArea.width(),
                       demoArea.top() + qBound(0.0, worldPosition.y(), 1.0) * demoArea.height());
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
