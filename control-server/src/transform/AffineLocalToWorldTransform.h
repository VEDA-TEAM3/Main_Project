#pragma once

/**
 * @file    AffineLocalToWorldTransform.h
 * @brief   채널별 위치/방향으로 로컬 좌표를 월드 좌표로 회전·이동 변환
 */

#include <vector>

#include "core/AppConfig.h"
#include "interfaces/ILocalToWorldTransform.h"

/**
 * @brief 채널별 위치/방향으로 로컬 좌표를 월드 좌표로 회전·이동 변환
 */
class AffineLocalToWorldTransform : public ILocalToWorldTransform {
public:
    explicit AffineLocalToWorldTransform(std::vector<CameraCalibration> calibrations);
    void transform(std::vector<veda::TopViewFrame>& frames) override;

private:
    std::vector<CameraCalibration> calibrations_;
    const CameraCalibration* find(veda::ChannelId ch) const;
};