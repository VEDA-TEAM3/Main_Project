#pragma once

#include <QRectF>

class QGraphicsScene;

class DigitalTwinMapSceneBuilder {
public:
    virtual ~DigitalTwinMapSceneBuilder() = default;

    virtual QRectF build(QGraphicsScene* scene) const = 0;
};

class DemoParkingMapSceneBuilder final : public DigitalTwinMapSceneBuilder {
public:
    QRectF build(QGraphicsScene* scene) const override;
};
