#pragma once

/**
 * @file NullTransform.h
 * @brief ICoordinateTransform의 Stub 구현체. 입력 좌표를 그대로 반환.
 *
 * @details
 * 실제 호모그래피 캘리브레이션 행렬이 적용되기 전까지,
 * 파이프라인 컴파일 및 End-to-End 테스트를 위해 사용되는 항등 변환기.
 */

#include "interfaces/ICoordinateTransform.h"

/**
 * @class NullTransform
 * @brief 정규화 이미지 좌표를 물리적 변환 없이 그대로 월드 좌표 구조체에 담아 반환
 */
class NullTransform : public ICoordinateTransform {
public:
    NullTransform() = default;
    ~NullTransform() override = default;

    /**
     * @brief 이미지 좌표(u, v)를 월드 좌표(x, y)로 그대로 복사.
     * @param p IGroundPointExtractor가 추출한 정규화 이미지 좌표 [0,1]
     * @return veda::WorldPoint 변환 없이 그대로 매핑된 좌표. (단위는 미터가 아니라 [0,1] 상태임)
     */
    veda::WorldPoint toWorld(const domain::ImagePoint& p) override;
};