#pragma once

/**
 * @file    NullTransform.h
 * @brief   입력 좌표를 그대로 반환 (Debug)
 */

#include "interfaces/ICoordinateTransform.h"

/**
 * @brief   입력 좌표를 그대로 반환
 */
class NullTransform : public ICoordinateTransform {
public:
    NullTransform() = default;
    ~NullTransform() override = default;

    /**
     * @brief   이미지 좌표(u, v)를 월드 좌표(x, y)로 그대로 복사
     * @param   p IGroundPointExtractor가 추출한 정규화 이미지 좌표 [0,1]
     * @return  변환 없이 그대로 매핑된 좌표
     */
    std::optional<veda::WorldPoint> toWorld(const domain::ImagePoint& p) override;
};