#pragma once

/**
 * @file    AffineImageCoordinateMapper.h
 * @brief   scale, offset (blur 경로 전용)
 */

#include "interfaces/IImageCoordinateMapper.h"

class AffineImageCoordinateMapper final : public IImageCoordinateMapper {
public:
    AffineImageCoordinateMapper(double scaleX, double scaleY, double offsetX, double offsetY);
    void map(std::vector<domain::DetectedObject>& objects, veda::ChannelId channelId) const override;

private:
    double scaleX_;
    double scaleY_;
    double offsetX_;
    double offsetY_;
};