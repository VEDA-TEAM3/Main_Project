#pragma once

#include <QVector>

#include "model/DigitalTwinTypes.h"

class DigitalTwinObjectSpawner {
public:
    virtual ~DigitalTwinObjectSpawner() = default;

    virtual QVector<DigitalTwinObject> createInitialObjects(int objectCount) = 0;
    virtual DigitalTwinObject createEnteringObject() = 0;
    virtual int nextSpawnDelayMsec() const = 0;
};

class RandomEdgeObjectSpawner final : public DigitalTwinObjectSpawner {
public:
    QVector<DigitalTwinObject> createInitialObjects(int objectCount) override;
    DigitalTwinObject createEnteringObject() override;
    int nextSpawnDelayMsec() const override;

private:
    DigitalTwinObject createObject(DigitalTwinObjectType objectType, const QPointF& position, const QPointF& velocity);
    QString nextObjectId(DigitalTwinObjectType objectType);

    int nextSequence_ = 1;
};
