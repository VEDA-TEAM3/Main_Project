#pragma once

#include <QRectF>
#include <QVector>

class QGraphicsScene;

struct DigitalTwinMapSceneLayout {
    QRectF sceneRect;
    /// 물리 CCTV 한 대가 담당하는 지도 영역
    QVector<QRectF> zoneRects;
    /// 구역별 장치 상태 칩 자리. 윗줄 구역은 도면 위, 아랫줄 구역은 도면 아래에 놓인다
    QVector<QRectF> zoneStatusSlots;
};

class DigitalTwinMapSceneBuilder {
public:
    virtual ~DigitalTwinMapSceneBuilder() = default;

    /**
     * @brief                  고정 지도 요소를 구성합니다.
     * @param scene            아이템을 추가할 scene
     * @param activeZoneCount  CCTV가 실제로 붙어 있는 구역 수. 나머지 자리는 확장용으로 남긴다
     */
    virtual DigitalTwinMapSceneLayout build(QGraphicsScene* scene, int activeZoneCount) const = 0;
};

class DemoParkingMapSceneBuilder final : public DigitalTwinMapSceneBuilder {
public:
    DigitalTwinMapSceneLayout build(QGraphicsScene* scene, int activeZoneCount) const override;
};
