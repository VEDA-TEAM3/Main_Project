#pragma once

/**
 * @file    IClock.h
 * @brief   시스템 시간 획득을 위한 추상화 인터페이스
 *
 * @details
 * FrameAggregator의 시간 윈도우 로직을 하드웨어 시간에 종속시키지 않고,
 * 가짜 시간을 주입하여 안정적으로 단위 테스트하기 위해 사용
 */

#include "Contract.h"

class IClock {
public:
    virtual ~IClock() = default;

    /**
     * @brief   현재 시간을 밀리초(ms) 단위로 반환
     * @return  veda::TimestampMs 현재 타임스탬프
     */
    virtual veda::TimestampMs now() const = 0;
};