#pragma once

/**
 * @file    IDistanceMetric.h
 * @brief   객체 간 물리적 거리 계산을 위한 추상화 인터페이스
 *
 * @details
 * 다양한 거리 계산 공식을 유연하게 교체할 수 있도록
 * 수학적 측정 로직을 비즈니스 로직과 분리
 */

#include "domain/WorldPoint.h"

class IDistanceMetric {
public:
    virtual ~IDistanceMetric() = default;

    /**
     * @brief   두 지점 간의 물리적 거리(m)를 계산
     * @param   p1 첫 번째 지점
     * @param   p2 두 번째 지점
     * @return 계산된 물리적 거리 (단위: m)
     */
    virtual double calculate(const domain::WorldPoint& p1, const domain::WorldPoint& p2) const = 0;
};