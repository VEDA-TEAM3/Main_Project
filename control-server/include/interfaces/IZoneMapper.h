#pragma once

/**
 * @file    IZoneMapper.h
 * @brief   월드 좌표를 물리적 액추에이터 채널(zone)에 배정하는 인터페이스
 *
 * @details
 * 카메라 FOV 겹침과 무관하게, 도면상 배타적 각도/영역 구간으로 좌표를 채널에 매핑
 * ICrossChannelFuser의 dedup 이후, IRiskPolicy 이전 단계에서 실행되어야 함
 * (경계 근처 객체가 dedup 전에 서로 다른 zone 으로 갈리는 것을 방지)
 */

#include "domain/WorldFrame.h"

class IZoneMapper {
public:
    virtual ~IZoneMapper() = default;

    /**
     * @brief   frame 내 각 객체의 zoneId 를 배정
     * @param   frame 배정 대상 프레임
     * @note    IRiskPolicy::evaluate와 동일하게 mutate 패턴 사용
     */
    virtual void assign(domain::WorldFrame& frame) = 0;
};