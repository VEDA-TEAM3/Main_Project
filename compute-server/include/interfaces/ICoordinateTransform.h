#pragma once

/**
 * @file    ICoordinateTransform.h
 * @brief   정규화 이미지 좌표를 월드 좌표로 변환하는 인터페이스
 */

#include <optional>

#include "Contract.h"
#include "interfaces/IGroundPointExtractor.h"

/**
 * @brief 이미지 픽셀/정규화 좌표를 실제 물리적 공간의 좌표계로 변환하는 역할을 수행하는 인터페이스
 */
class ICoordinateTransform {
public:
    virtual ~ICoordinateTransform() = default;

    /**
     * @brief   2D 이미지 평면 상의 좌표를 물리적 공간의 월드 좌표로 사상
     *
     * @param   p 변환할 정규화 이미지 좌표
     * @return  행렬 변환 등이 적용된 실제 물리적 월드 좌표 (m 단위)
     */
    virtual std::optional<veda::WorldPoint> toWorld(const domain::ImagePoint& p) = 0;
};