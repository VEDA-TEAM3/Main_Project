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

    QTimer* updateTimer_ = nullptr;
    std::shared_ptr<DigitalTwinRiskPolicy> riskPolicy_;
    std::shared_ptr<DigitalTwinObjectSpawner> objectSpawner_;
    QVector<DigitalTwinObject> objects_;
    QVector<DigitalTwinPairRiskState> pairRiskStates_;
    QHash<QString, DigitalTwinRiskLevel> previousPairRiskLevels_;
    QHash<QString, int> pairPulseCooldownTicks_;
    int spawnCountdownMsec_ = 0;
};
