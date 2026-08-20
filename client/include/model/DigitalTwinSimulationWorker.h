#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <memory>

#include "model/DigitalTwinTypes.h"

class DigitalTwinObjectSpawner;
class DigitalTwinRiskPolicy;

class DigitalTwinSimulationWorker : public QObject {
    Q_OBJECT

public:
    explicit DigitalTwinSimulationWorker(QObject* parent = nullptr);
    explicit DigitalTwinSimulationWorker(std::shared_ptr<DigitalTwinRiskPolicy> riskPolicy, QObject* parent = nullptr);
    explicit DigitalTwinSimulationWorker(std::shared_ptr<DigitalTwinRiskPolicy> riskPolicy,
                                         std::shared_ptr<DigitalTwinObjectSpawner> objectSpawner,
                                         QObject* parent = nullptr);

    /**
     * @brief            데모가 객체를 돌아다니게 할 활성 구역 수를 정합니다.
     * @param zoneCount  활성 구역 수 (1 미만이면 1로 봅니다)
     *
     * @details worker를 스레드로 옮기고 start()를 부르기 **전에** 호출하세요. 데모 전용
     *          값이라 실 데이터 경로에는 아무 영향이 없습니다.
     */
    void setZoneCount(int zoneCount);

public slots:
    void start();
    void stop();

signals:
    void snapshotUpdated(DigitalTwinSnapshot snapshot);
    void riskEventDetected(DigitalTwinRiskEvent event);

private slots:
    void updateObjects();

private:
    void setupDemoObjects();
    void ensureTimer();
    void updateObjectMotion(DigitalTwinObject* object);
    void removeExitedObjects();
    void spawnObjectIfNeeded();
    void scheduleNextSpawn();
    void updateRiskLevels();
    void emitCurrentSnapshot();
    double globalXForObject(const DigitalTwinObject& object) const;
    void applyGlobalX(DigitalTwinObject* object, double globalX) const;
    double globalXFromSpawnerX(double spawnerX) const;

    QTimer* updateTimer_ = nullptr;
    std::shared_ptr<DigitalTwinRiskPolicy> riskPolicy_;
    std::shared_ptr<DigitalTwinObjectSpawner> objectSpawner_;
    QVector<DigitalTwinObject> objects_;
    QVector<DigitalTwinPairRiskState> pairRiskStates_;
    QHash<QString, DigitalTwinRiskLevel> previousPairRiskLevels_;
    QHash<QString, int> pairPulseCooldownTicks_;
    qint64 sampleSequence_ = 0;
    int spawnCountdownMsec_ = 0;
    int zoneCount_ = 1;
};
