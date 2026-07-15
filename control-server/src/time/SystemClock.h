#pragma once

/**
 * @file SystemClock.h
 * @brief IClock 인터페이스의 실제 시스템 시간 구현체
 * @details 실제 운영 환경에서 NTP로 동기화된 시스템 표준 시간을 반환
 */

#include "interfaces/IClock.h"

class SystemClock : public IClock {
public:
    SystemClock() = default;
    ~SystemClock() override = default;

    /**
     * @brief 현재 시스템 시간을 Epoch 기준 밀리초(ms)로 반환
     */
    veda::TimestampMs now() const override;
};