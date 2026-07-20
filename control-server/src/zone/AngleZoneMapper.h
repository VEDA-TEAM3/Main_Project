#pragma once

/**
 * @file    AngleZoneMapper.h
 */

#include <memory>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/IZoneMapper.h"

/**
 * @brief atan2 기반 각도 구간으로 좌표를 zone에 배정하는 구현체
 * @details
 * 원점 = 사거리 중심(CCTV 설치 위치)
 * AppConfig::zoneBoundaries의 각 채널별 [angleMinDeg, angleMaxDeg)구간에
 * 좌표의 각도가 속하면 그 채널로 배정
 * 등분이 아닌 임의 구간을 지원 (합 360도 가정, wrap-around 처리 포함)
 */
class AngleZoneMapper : public IZoneMapper {
public:
    explicit AngleZoneMapper(std::vector<ZoneBoundary> boundaries);
    ~AngleZoneMapper() override = default;

    void assign(domain::WorldFrame& frame) override;

private:
    std::vector<ZoneBoundary> boundaries_;
};