#pragma once

/**
 * @file    AffineImageCoordinateMapper.h
 * @brief   
 */

#include "interfaces/IImageCoordinateMapper.h"

class AffineImageCoordinateMapper final : public IImageCoordinateMapper {
public:
    AffineImageCoordinateMapper(double scaleX, double scaleY, double offsetX, double offsetY);
    domain::ChannelFrame map(domain::ChannelFrame frame) const override;

private:
    double scaleX_;
    double scaleY_;
    double offsetX_;
    double offsetY_;
};