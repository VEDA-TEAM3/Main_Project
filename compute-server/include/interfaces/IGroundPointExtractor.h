#pragma once

/**
 * @file IGroundPointExtractor.h
 * @brief BBox 에서 지면 접촉점을 추출하는 인터페이스
 *
 * @note [지면점 추출이 반드시 필요한 이유]
 * - Homography 평면 제약: 모든 점은 동일한 평면 위에 존재해야 함
 * - 시차 오류: 공중에 약간 뜬 상태로 변환 행렬에 넣고 돌리면,
 *   바깥쪽으로 크게 밀려나는 왜곡이 발생하여 실제 위치와 수 미터 오차 발생
 * - 잘림 현상: 객체가 화면 경계에 잘렸다면, 카메라 바로 밑에 있음에도 불구하고
 *   훨씬 멀리 있는 것으로 오판
 */

#include "domain/NormBox.h"

namespace domain {
/**
 * @brief 정규화 이미지 평면 위의 2D 좌표
 */
struct ImagePoint {
    double u = 0.0;  ///< [0,1], 좌상단 원점 기준 가로 축
    double v = 0.0;  ///< [0,1], 좌상단 원점 기준 세로 축
};
}  // namespace domain

/**
 * @brief 객체의 BBox로부터 지면과 맞닿는 기준점을 계산하는 인터페이스
 */
class IGroundPointExtractor {
public:
    virtual ~IGroundPointExtractor() = default;

    /**
     * @brief 정규화된 BBox 정보를 바탕으로 지면 접촉점으로 간주할 좌표를 추출.
     *
     * @param box 추출 대상 객체의 정규화된 BBox 정보
     * @return ImagePoint 이미지 평면 상의 추출된 지면 좌표 (월드 좌표 아님)
     */
    virtual domain::ImagePoint extract(const domain::NormBox& box) = 0;
};