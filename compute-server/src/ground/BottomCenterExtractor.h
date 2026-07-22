#pragma once

/**
 * @file    BottomCenterExtractor.h
 * @brief   bbox 하단 중앙을 지면 접촉점으로 사용
 */

#include "interfaces/IGroundPointExtractor.h"

/**
 * @class   BottomCenterExtractor
 * @brief   bbox 하단 중앙 { (l+r)/2, b } 를 지면 접촉점으로 추출
 *
 * @details
 * bbox 중심점(CoG)을 쓰지 않음
 * 사람은 허리~가슴 높이, 차량은 차체 중간 높이가 지면이 아니므로,
 * 지면 평면만 사상하는 호모그래피에 그 점을 넣으면 CCTV에서
 * 먼 방향으로 체계적인 오차가 발생
 */
class BottomCenterExtractor : public IGroundPointExtractor {
public:
    domain::ImagePoint extract(const domain::NormBox& box) override;
};