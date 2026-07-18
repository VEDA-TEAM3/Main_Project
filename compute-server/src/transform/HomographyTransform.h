#pragma once

/**
 * @file    HomographyTransform.h
 * @brief   2D좌표 → 월드 좌표 (Homography)
 */

#include <array>

#include "interfaces/ICoordinateTransform.h"

class HomographyTransform final : public ICoordinateTransform {
public:
    explicit HomographyTransform(std::array<double, 9> matrix);
    std::optional<veda::WorldPoint> toWorld(const domain::ImagePoint& p) override;

private:
    std::array<double, 9> matrix_;
};