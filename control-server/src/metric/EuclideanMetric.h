#pragma once

/**
 * @file EuclideanMetric.h
 * @brief 유클리드 거리 계산 구현체
 */

#include "interfaces/IDistanceMetric.h"

class EuclideanMetric : public IDistanceMetric {
public:
    EuclideanMetric() = default;
    ~EuclideanMetric() override = default;

    /**
     * @brief 두 2D 좌표 간의 유클리드 직선 거리를 계산
     */
    double calculate(const domain::WorldPoint& p1, const domain::WorldPoint& p2) const override;
};