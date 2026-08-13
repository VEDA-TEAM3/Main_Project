#pragma once

#include <QRectF>
#include <array>

class QGraphicsScene;

struct DigitalTwinMapSceneLayout {
    QRectF sceneRect;
    /// 물리 CCTV 한 대가 담당하는 지도 영역
    std::array<QRectF, 2> zoneRects;
    /// 구역별 장치 상태 칩 자리. 윗줄 구역은 도면 위, 아랫줄 구역은 도면 아래에 놓인다
    std::array<QRectF, 2> zoneStatusSlots;
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
