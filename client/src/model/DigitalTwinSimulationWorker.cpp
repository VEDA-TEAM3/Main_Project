#include "model/DigitalTwinSimulationWorker.h"

#include <QRandomGenerator>
#include <QtGlobal>
#include <algorithm>

#include "model/DigitalTwinObjectSpawner.h"
#include "model/DigitalTwinRiskPolicy.h"

namespace {
constexpr int updateIntervalMsec = 50;
constexpr double objectMinY = 0.14;
constexpr double objectMaxY = 0.86;
constexpr double horizontalEdgeTransitionPadding = 0.02;
constexpr double maximumVelocityPerSecond = 0.13;
constexpr double minimumHorizontalVelocityPerSecond = 0.038;
constexpr double horizontalVelocityJitterPerSecond = 0.01;
constexpr double verticalVelocityJitterPerSecond = 0.02;
constexpr int directionChangeProbabilityPercent = 12;
constexpr int warningPulseRepeatMsec = 1200;
constexpr int dangerPulseRepeatMsec = 1500;
constexpr int fadeInDurationMsec = 120;
constexpr int initialObjectCount = 5;

constexpr double secondsPerUpdate = static_cast<double>(updateIntervalMsec) / 1000.0;
constexpr double maxVelocityPerTick = maximumVelocityPerSecond * secondsPerUpdate;
constexpr double minimumHorizontalVelocity = minimumHorizontalVelocityPerSecond * secondsPerUpdate;
constexpr double horizontalVelocityJitter = horizontalVelocityJitterPerSecond * secondsPerUpdate;
constexpr double verticalVelocityJitter = verticalVelocityJitterPerSecond * secondsPerUpdate;
constexpr double opacityIncrementPerTick = static_cast<double>(updateIntervalMsec) / fadeInDurationMsec;

/** @brief 데모 객체를 8개 서버 채널에 안정적으로 분산합니다. */
int demoChannelIndex(const QString& objectId) { return static_cast<int>(qHash(objectId) % 8U); }

/**
 * @brief               지정 범위 안의 난수를 생성합니다.
 * @param minimumValue  최소값
 * @param maximumValue  최대값
 * @return              범위 안의 임의 실수
 */
double randomRange(double minimumValue, double maximumValue) {
    return minimumValue + (maximumValue - minimumValue) * QRandomGenerator::global()->generateDouble();
}

/**
 * @brief        시뮬레이션 계산용 절댓값을 반환합니다.
 * @param value  입력 값
 * @return       절댓값
 */
double simulationAbsoluteValue(double value) { return value < 0.0 ? -value : value; }

/**
 * @brief           속도 벡터의 최대 성분을 제한합니다.
 * @param velocity  제한 전 속도 벡터
 * @return          최대 이동량을 넘지 않는 속도 벡터
 */
QPointF limitedVelocity(const QPointF& velocity) {
    const double maxComponent = std::max(simulationAbsoluteValue(velocity.x()), simulationAbsoluteValue(velocity.y()));

    if (maxComponent <= maxVelocityPerTick || qFuzzyIsNull(maxComponent)) {
        return velocity;
    }

    const double scale = maxVelocityPerTick / maxComponent;
    return QPointF(velocity.x() * scale, velocity.y() * scale);
}

/**
 * @brief        값의 부호를 반환합니다.
 * @param value  입력 값
 * @return       0 이상이면 1.0, 음수이면 -1.0
 */
double directionSign(double value) { return value < 0.0 ? -1.0 : 1.0; }

/**
 * @brief           위험 단계 비교를 위한 우선순위를 반환합니다.
 * @param riskLevel  위험 단계
 * @return           Normal < Warning < Danger 순서의 정수 우선순위
 */
int riskPriority(DigitalTwinRiskLevel riskLevel) {
    switch (riskLevel) {
        case DigitalTwinRiskLevel::Normal:
            return 0;
        case DigitalTwinRiskLevel::Warning:
            return 1;
        case DigitalTwinRiskLevel::Danger:
            return 2;
    }

    return 0;
}

/**
 * @brief           위험 단계별 오버레이 반복 간격 tick 수를 반환합니다.
 * @param riskLevel  위험 단계
 * @return           반복 간격 tick 수
 */
int pulseRepeatTicksForRiskLevel(DigitalTwinRiskLevel riskLevel) {
    if (riskLevel == DigitalTwinRiskLevel::Danger) {
        return (dangerPulseRepeatMsec + updateIntervalMsec - 1) / updateIntervalMsec;
    }

    if (riskLevel == DigitalTwinRiskLevel::Warning) {
        return (warningPulseRepeatMsec + updateIntervalMsec - 1) / updateIntervalMsec;
    }

    return 0;
}

/**
 * @brief               위험 단계에서 주의 단계로 낮아지는 전환인지 확인합니다.
 * @param previousRisk  이전 tick의 객체 쌍 위험 단계
 * @param currentRisk   현재 tick의 객체 쌍 위험 단계
 * @return              위험에서 주의로 하향 전환되면 true
 */
bool isDangerToWarningDowngrade(DigitalTwinRiskLevel previousRisk, DigitalTwinRiskLevel currentRisk) {
    return previousRisk == DigitalTwinRiskLevel::Danger && currentRisk == DigitalTwinRiskLevel::Warning;
}

/**
 * @brief               두 객체 조합을 나타내는 안정적인 key를 생성합니다.
 * @param firstObject   첫 번째 객체
 * @param secondObject  두 번째 객체
 * @return              객체 순서와 무관한 pair key
 */
QString pairKeyForObjects(const DigitalTwinObject& firstObject, const DigitalTwinObject& secondObject) {
    if (firstObject.objectId < secondObject.objectId) {
        return firstObject.objectId + QStringLiteral("|") + secondObject.objectId;
    }

    return secondObject.objectId + QStringLiteral("|") + firstObject.objectId;
}

/**
 * @brief               두 객체 사이의 중간 위치를 계산합니다.
 * @param firstObject   첫 번째 객체
 * @param secondObject  두 번째 객체
 * @return              정규화 좌표계 기준 중간점
 */
QPointF midpointForObjects(const DigitalTwinObject& firstObject, const DigitalTwinObject& secondObject) {
    return QPointF((firstObject.position.x() + secondObject.position.x()) * 0.5,
                   (firstObject.position.y() + secondObject.position.y()) * 0.5);
}

}  // namespace

/**
 * @brief       디지털 트윈 객체 이동과 위험 판정을 수행하는 worker를
 * 생성합니다.
 * @param parent  Qt 객체 소유권 부모
 */
DigitalTwinSimulationWorker::DigitalTwinSimulationWorker(QObject* parent)
    : DigitalTwinSimulationWorker(std::make_shared<DistanceRiskPolicy>(), parent) {}

/**
 * @brief            위험 판정 정책을 주입받아 worker를 생성합니다.
 * @param riskPolicy  객체 간 위험 단계를 계산할 정책
 * @param parent      Qt 객체 소유권 부모
 */
DigitalTwinSimulationWorker::DigitalTwinSimulationWorker(std::shared_ptr<DigitalTwinRiskPolicy> riskPolicy,
                                                         QObject* parent)
    : DigitalTwinSimulationWorker(std::move(riskPolicy), std::make_shared<RandomEdgeObjectSpawner>(), parent) {}

/**
 * @brief               위험 판정 정책과 객체 생성 정책을 주입받아 worker를
 * 생성합니다.
 * @param riskPolicy     객체 간 위험 단계를 계산할 정책
 * @param objectSpawner  초기 객체와 경계 진입 객체를 생성할 정책
 * @param parent         Qt 객체 소유권 부모
 */
DigitalTwinSimulationWorker::DigitalTwinSimulationWorker(std::shared_ptr<DigitalTwinRiskPolicy> riskPolicy,
                                                         std::shared_ptr<DigitalTwinObjectSpawner> objectSpawner,
                                                         QObject* parent)
    : QObject(parent), riskPolicy_(std::move(riskPolicy)), objectSpawner_(std::move(objectSpawner)) {
    if (!riskPolicy_) {
        riskPolicy_ = std::make_shared<DistanceRiskPolicy>();
    }

    if (!objectSpawner_) {
        objectSpawner_ = std::make_shared<RandomEdgeObjectSpawner>();
    }
}

/**
 * @brief   데모 객체를 초기화하고 주기적인 상태 계산을 시작합니다.
 */
void DigitalTwinSimulationWorker::start() {
    ensureTimer();
    setupDemoObjects();
    updateRiskLevels();
    emitCurrentSnapshot();

    if (!updateTimer_->isActive()) {
        updateTimer_->start();
    }
}

/**
 * @brief   주기적인 상태 계산을 중지합니다.
 */
void DigitalTwinSimulationWorker::stop() {
    if (updateTimer_) {
        updateTimer_->stop();
    }
}

/**
 * @brief   모든 객체의 위치와 위험 단계를 한 tick만큼 갱신합니다.
 */
void DigitalTwinSimulationWorker::updateObjects() {
    for (auto& object : objects_) {
        updateObjectMotion(&object);
    }

    removeExitedObjects();
    spawnObjectIfNeeded();
    updateRiskLevels();
    emitCurrentSnapshot();
}

/**
 * @brief   데모용 객체 목록과 초기 이동 벡터를 구성합니다.
 */
void DigitalTwinSimulationWorker::setupDemoObjects() {
    objects_ = objectSpawner_->createInitialObjects(initialObjectCount);
    for (DigitalTwinObject& object : objects_) {
        object.channelIndex = demoChannelIndex(object.objectId);
        object.opacity = 1.0;
    }

    previousPairRiskLevels_.clear();
    pairPulseCooldownTicks_.clear();
    scheduleNextSpawn();
}

/**
 * @brief   worker가 속한 스레드에서 동작할 갱신 타이머를 준비합니다.
 */
void DigitalTwinSimulationWorker::ensureTimer() {
    if (updateTimer_) {
        return;
    }

    updateTimer_ = new QTimer(this);
    updateTimer_->setInterval(updateIntervalMsec);
    updateTimer_->setTimerType(Qt::PreciseTimer);

    connect(updateTimer_, &QTimer::timeout, this, &DigitalTwinSimulationWorker::updateObjects);
}

/**
 * @brief        단일 객체의 좌우 진행과 상하 경계 반사를 계산합니다.
 * @param object  갱신할 객체
 */
void DigitalTwinSimulationWorker::updateObjectMotion(DigitalTwinObject* object) {
    if (!object) {
        return;
    }

    if (QRandomGenerator::global()->bounded(100) < directionChangeProbabilityPercent) {
        const double horizontalDirection = directionSign(object->velocity.x());
        const double horizontalSpeed =
            std::max(minimumHorizontalVelocity, simulationAbsoluteValue(object->velocity.x()) +
                                                    randomRange(-horizontalVelocityJitter, horizontalVelocityJitter));
        object->velocity = limitedVelocity(
            QPointF(horizontalDirection * horizontalSpeed,
                    object->velocity.y() + randomRange(-verticalVelocityJitter, verticalVelocityJitter)));
    }

    QPointF nextPosition = object->position + object->velocity;

    if (nextPosition.y() < objectMinY || nextPosition.y() > objectMaxY) {
        object->velocity.setY(-object->velocity.y());
        nextPosition.setY(std::clamp(nextPosition.y(), objectMinY, objectMaxY));
    }

    object->position = nextPosition;

    const double fadeInOpacity = std::min(1.0, object->opacity + opacityIncrementPerTick);
    double edgeOpacity = 1.0;
    if (object->position.x() < 0.0) {
        edgeOpacity = std::clamp(
            (object->position.x() + horizontalEdgeTransitionPadding) / horizontalEdgeTransitionPadding, 0.0, 1.0);
    } else if (object->position.x() > 1.0) {
        edgeOpacity = std::clamp(
            (1.0 + horizontalEdgeTransitionPadding - object->position.x()) / horizontalEdgeTransitionPadding, 0.0, 1.0);
    }
    object->opacity = std::min(fadeInOpacity, edgeOpacity);
}

/**
 * @brief   좌우 맵 바깥으로 완전히 이탈한 객체를 제거합니다.
 */
void DigitalTwinSimulationWorker::removeExitedObjects() {
    for (qsizetype index = objects_.size() - 1; index >= 0; --index) {
        const DigitalTwinObject& object = objects_[index];
        const double x = object.position.x();
        const double velocityX = object.velocity.x();
        const bool leftExit = x <= -horizontalEdgeTransitionPadding && velocityX < 0.0;
        const bool rightExit = x >= 1.0 + horizontalEdgeTransitionPadding && velocityX > 0.0;

        if (leftExit || rightExit) {
            objects_.removeAt(index);
        }
    }
}

/**
 * @brief   스폰 카운트다운이 끝났을 때 좌우 경계에서 새 객체를 생성합니다.
 */
void DigitalTwinSimulationWorker::spawnObjectIfNeeded() {
    spawnCountdownMsec_ -= updateIntervalMsec;

    if (spawnCountdownMsec_ > 0) {
        return;
    }

    DigitalTwinObject object = objectSpawner_->createEnteringObject();
    object.channelIndex = demoChannelIndex(object.objectId);
    object.opacity = 0.0;
    objects_.append(std::move(object));
    scheduleNextSpawn();
}

/**
 * @brief   다음 객체 생성을 10~15초 사이 무작위 지연으로 예약합니다.
 */
void DigitalTwinSimulationWorker::scheduleNextSpawn() { spawnCountdownMsec_ = objectSpawner_->nextSpawnDelayMsec(); }

/**
 * @brief   현재 객체 간 거리만 기준으로 위험 단계를 재계산하고 오버레이
 * 이벤트를 발생시킵니다.
 */
void DigitalTwinSimulationWorker::updateRiskLevels() {
    for (auto& object : objects_) {
        object.riskLevel = DigitalTwinRiskLevel::Normal;
    }

    for (auto iterator = pairPulseCooldownTicks_.begin(); iterator != pairPulseCooldownTicks_.end();) {
        if (iterator.value() <= 1) {
            iterator = pairPulseCooldownTicks_.erase(iterator);
            continue;
        }

        --iterator.value();
        ++iterator;
    }

    QHash<QString, DigitalTwinRiskLevel> currentPairRiskLevels;
    const qsizetype objectCount = objects_.size();
    const qsizetype maximumPairCount = objectCount > 1 ? objectCount * (objectCount - 1) / 2 : 0;
    currentPairRiskLevels.reserve(maximumPairCount);
    pairRiskStates_.clear();
    pairRiskStates_.reserve(maximumPairCount);

    for (int firstIndex = 0; firstIndex < objects_.size(); ++firstIndex) {
        for (int secondIndex = firstIndex + 1; secondIndex < objects_.size(); ++secondIndex) {
            const int firstChannelIndex = objects_[firstIndex].channelIndex;
            const int secondChannelIndex = objects_[secondIndex].channelIndex;
            if (firstChannelIndex < 0 || secondChannelIndex < 0 || firstChannelIndex / 4 != secondChannelIndex / 4) {
                continue;
            }

            const DigitalTwinRiskLevel pairRiskLevel =
                riskPolicy_->riskLevelForObjects(objects_[firstIndex], objects_[secondIndex]);
            if (pairRiskLevel == DigitalTwinRiskLevel::Normal) {
                continue;
            }

            const QString pairKey = pairKeyForObjects(objects_[firstIndex], objects_[secondIndex]);
            currentPairRiskLevels.insert(pairKey, pairRiskLevel);

            DigitalTwinPairRiskState pairRiskState;
            pairRiskState.firstObjectId = objects_[firstIndex].objectId;
            pairRiskState.secondObjectId = objects_[secondIndex].objectId;
            pairRiskState.riskLevel = pairRiskLevel;
            pairRiskStates_.append(std::move(pairRiskState));

            if (riskPriority(pairRiskLevel) > riskPriority(objects_[firstIndex].riskLevel)) {
                objects_[firstIndex].riskLevel = pairRiskLevel;
            }

            if (riskPriority(pairRiskLevel) > riskPriority(objects_[secondIndex].riskLevel)) {
                objects_[secondIndex].riskLevel = pairRiskLevel;
            }

            const DigitalTwinRiskLevel previousRiskLevel =
                previousPairRiskLevels_.value(pairKey, DigitalTwinRiskLevel::Normal);
            const bool riskLevelChanged = pairRiskLevel != previousRiskLevel;
            const bool cooldownFinished = pairPulseCooldownTicks_.value(pairKey, 0) <= 0;

            if (isDangerToWarningDowngrade(previousRiskLevel, pairRiskLevel)) {
                pairPulseCooldownTicks_.insert(pairKey, pulseRepeatTicksForRiskLevel(pairRiskLevel));
                continue;
            }

            if (riskLevelChanged || cooldownFinished) {
                DigitalTwinRiskEvent riskEvent;
                riskEvent.firstObjectId = objects_[firstIndex].objectId;
                riskEvent.secondObjectId = objects_[secondIndex].objectId;
                riskEvent.position = midpointForObjects(objects_[firstIndex], objects_[secondIndex]);
                riskEvent.riskLevel = pairRiskLevel;
                riskEvent.channelIndex = firstChannelIndex;

                emit riskEventDetected(riskEvent);
                pairPulseCooldownTicks_.insert(pairKey, pulseRepeatTicksForRiskLevel(pairRiskLevel));
            }
        }
    }

    previousPairRiskLevels_ = currentPairRiskLevels;
}

/**
 * @brief   최신 객체와 객체 쌍 위험 상태를 하나의 스냅샷으로 UI 스레드에
 * 전달합니다.
 */
void DigitalTwinSimulationWorker::emitCurrentSnapshot() {
    DigitalTwinSnapshot snapshot;
    snapshot.objects = objects_;
    snapshot.pairRiskStates = pairRiskStates_;
    emit snapshotUpdated(std::move(snapshot));
}
