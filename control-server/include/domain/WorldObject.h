#pragma once

/**
 * @file    WorldObject.h
 * @brief   월드 좌표 내의 객체
 */

#include <vector>

#include "Contract.h"
#include "domain/WorldPoint.h"

namespace domain {

struct WorldObject {
    veda::GlobalId gid = 0;
    veda::ObjectClass cls = veda::ObjectClass::Unknown;
    WorldPoint pos;

    /**
     * @brief 위험 레벨
     *
     * @note
     * - 주의: cls에 따라 의미가 달라짐
     * -- Vehicle:  차량은 평가의 대상
     * -- Human:    스스로 평가되지 않으며, 차량의 nearestObj로 지목된 경우 채워짐
     */
    veda::RiskLevel riskLevel = veda::RiskLevel::None;

    veda::GlobalId nearestObj = 0;
    double nearestDist = -1.0;

    veda::ChannelId zoneId = -1;

    std::vector<veda::ChannelId> sourceChannels;  ///< dedup 병합되면 2개 이상
};

}  // namespace domain