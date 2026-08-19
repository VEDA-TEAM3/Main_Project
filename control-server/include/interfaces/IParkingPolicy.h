#pragma once

/**
 * @file    IParkingPolicy.h
 * @brief   월드 객체의 주차 상태를 판정하고 후속 처리 대상을 선별하는 인터페이스
 */

#include "domain/WorldFrame.h"

class IParkingPolicy {
public:
    virtual ~IParkingPolicy() = default;

    /**
     * @brief 주차 정책을 적용해 Top-View 및 위험 판단에서 제외할 객체를 frame에서 제거
     * @param frame 교차 채널 융합이 끝난 현재 월드 프레임
     * @pre GridFuser가 안정적인 gid와 월드 좌표를 부여한 뒤 호출해야 함
     */
    virtual void apply(domain::WorldFrame& frame) = 0;
};
