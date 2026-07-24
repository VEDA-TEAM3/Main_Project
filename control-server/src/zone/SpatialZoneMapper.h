#pragma once

/**
 * @file    SpatialZoneMapper.h
 */

#include <vector>

#include "core/AppConfig.h"
#include "interfaces/IZoneMapper.h"

/**
 * @brief 월드 좌표를 사전 계산된 축 정렬 경계 상자(AABB)로 zone 에 배정하는 구현체
 *
 * @details
 * 전역 주차장 도면 위의 SpatialZone 목록에 대해 각 객체의 월드 좌표 (x,y) 를 검사한다.
 *  - first-match wins: zones 선언 순서대로 검사해 처음 포함되는 상자의 zoneId 를 배정
 *  - 경계 포함([minX,maxX] × [minY,maxY])
 *  - 어느 상자에도 안 들면 zoneId = -1 (미배정 → 하위 단계에서 알람 대상 제외)
 *
 * 각도 기반 AngleZoneMapper 를 대체한다. 각도는 '원점에서의 방향'만 보므로 카메라가 도면
 * 전역에 흩어진 글로벌 맵에서는 100m 떨어진 다른 구역이 같은 각도로 겹쳐 엉뚱한 zone 에
 * 배정됐다. 공간 상자는 객체의 '실제 위치'로 배정하므로 이 문제가 사라진다.
 */
class SpatialZoneMapper : public IZoneMapper {
public:
    explicit SpatialZoneMapper(std::vector<SpatialZone> zones);
    ~SpatialZoneMapper() override = default;

    void assign(domain::WorldFrame& frame) override;

private:
    std::vector<SpatialZone> zones_;
};
