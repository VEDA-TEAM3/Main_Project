#pragma once

/**
 * @file    AffineImageCoordinateMapper.h
 * @brief   scale, offset (Blur 경로 전용)
 */

#include "interfaces/IImageCoordinateMapper.h"

class AffineImageCoordinateMapper final : public IImageCoordinateMapper {
public:
    /**
     * @param boxScale  Blur 박스 확대 배율 (중심 고정, 폭/높이에만 적용)
     *                  1.0 미만은 박스를 줄여 대상을 노출시키므로 생성자에서 거부한다.
     */
    AffineImageCoordinateMapper(double scaleX, double scaleY, double offsetX, double offsetY, double boxScale = 1.0);
    void map(std::vector<domain::DetectedObject>& objects, veda::ChannelId channelId) const override;

private:
    double scaleX_;
    double scaleY_;
    double offsetX_;
    double offsetY_;
    double boxScale_;
};