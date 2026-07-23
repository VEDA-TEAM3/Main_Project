#pragma once

/**
 * @file    AffineLocalToWorldTransform.h
 * @brief   채널별 위치/방향으로 로컬 좌표를 월드 좌표로 회전·이동 변환
 */

#include <cstdint>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/ILocalToWorldTransform.h"

/**
 * @brief 채널별 위치/방향으로 로컬 좌표를 월드 좌표로 회전·이동 변환
 */
class AffineLocalToWorldTransform : public ILocalToWorldTransform {
public:
    /**
     * @param calibrations     채널별 카메라 캘리브레이션
     * @param dropUncalibrated 캘리브레이션이 없는 채널의 객체를 버릴지 여부 (기본 true)
     *                         false 면 로컬 좌표가 도면 좌표인 척 그대로 통과함 -- 위험
     */
    explicit AffineLocalToWorldTransform(std::vector<CameraCalibration> calibrations, bool dropUncalibrated = true);
    void transform(std::vector<veda::TopViewFrame>& frames) override;

private:
    std::vector<CameraCalibration> calibrations_;
    bool dropUncalibrated_;
    const CameraCalibration* find(veda::ChannelId ch) const;

    /// @brief 캘리브레이션 누락 로그 rate-limit 용 (윈도우마다 반복되므로)
    std::uint64_t uncalibratedCount_ = 0;
};