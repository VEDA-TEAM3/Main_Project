#pragma once

#include <QRectF>
#include <array>

class QGraphicsScene;

struct DigitalTwinMapSceneLayout {
    QRectF sceneRect;
    std::array<QRectF, 2> zoneRects;
};

class DigitalTwinMapSceneBuilder {
public:
    virtual ~DigitalTwinMapSceneBuilder() = default;

    virtual DigitalTwinMapSceneLayout build(QGraphicsScene* scene) const = 0;
};

class DemoParkingMapSceneBuilder final : public DigitalTwinMapSceneBuilder {
public:
    DigitalTwinMapSceneLayout build(QGraphicsScene* scene) const override;
};
