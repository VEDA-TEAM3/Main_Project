#pragma once

/**
 * @file    NullZoneMapper.h
 * @brief   Pipeline 검증용 가짜 구현체
 */

#include "interfaces/IZoneMapper.h"

/**
 * @brief   Pipeline 검증용 가짜 구현체
 * @details 모든 객체를 zoneId = 0으로 고정 배정
 */
class NullZoneMapper : public IZoneMapper {
public:
    void assign(domain::WorldFrame& frame) override;
};