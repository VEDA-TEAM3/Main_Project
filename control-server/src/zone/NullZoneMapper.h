#pragma once

#include "interfaces/IZoneMapper.h"

/**
 * @brief IZoneMapper 의 파이프라인 검증용 더미 구현체
 * @details 모든 객체를 zoneId = 0 으로 고정 배정
 */
class NullZoneMapper : public IZoneMapper {
public:
    void assign(domain::WorldFrame& frame) override;
};