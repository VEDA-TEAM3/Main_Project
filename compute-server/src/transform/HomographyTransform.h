#pragma once

#include <array>

#include "interfaces/ICoordinateTransform.h"

class HomographyTransform final : public ICoordinateTransform {
public:
    explicit HomographyTransform(std::array<double, 9> matrix);
    veda::WorldPoint toWorld(const domain::ImagePoint& p) override;

private:
    std::array<double, 9> matrix_;
};
