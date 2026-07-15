#pragma once

/**
 * @file ISink.h
 * @brief 최종 관제 데이터 전송 인터페이스
 * @details
 * 객체 융합 및 위험도 판정까지 모두 완료된 최종 도면 데이터를
 * 시각화를 담당하는 외부 시스템으로 전송
 */

#include "domain/WorldFrame.h"

class ISink {
public:
    virtual ~ISink() = default;

    /**
     * @brief 최종 프레임 데이터를 외부 대시보드로 전송
     * @param frame 모든 처리가 완료된 교차로 전체 프레임
     */
    virtual void send(const domain::WorldFrame& frame) = 0;
};