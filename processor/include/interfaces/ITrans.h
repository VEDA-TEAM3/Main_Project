/**
 * @file    ITrans.h
 * @brief   정규화된 카메라 좌표를 Top-view 좌표로 변환하는 인터페이스
 */
#pragma once

#include <memory>

/**
 * @brief   변환된 Top-view 좌표
 */
struct WorldPoint {
    double x = 0, y = 0; /**< Top-view 평면 상의 좌표 (단위: 확인 필요, dstPts 캘리브레이션 좌표계에 종속) */
    double distance = 0; /**< 이 카메라의 실세계 위치로부터의 유클리드 거리 */
};

/**
 * @brief   정규화된 카메라 이미지 좌표를 Top-view 좌표로 변환하는 인터페이스
 *
 * @details Homography 계산 방식이 변경될 가능성에 대비하여 인터페이스로 분리
 *          입력 좌표(x, y)는 Parser 단계에서 이미 정규화된 값(-1.0 ~ 1.0,
 *          화면 중앙이 0.0)이어야 하며, 픽셀 좌표를 그대로 넘겨서는 안 됨
 *
 *          여러 대의 카메라로 확장 가능하도록, WorldPoint::distance는 원점(0,0)이
 *          아니라 "이 ITrans 구현체가 속한 카메라의 실세계 위치"로부터의 거리를 의미
 *          카메라 위치를 어떻게 설정/갱신하는지는 구현체별 고유 API에
 *          위임 (예: HomographyTrans::setCameraPosition()).
 *
 *          구현체가 캘리브레이션되지 않은 상태(예: Homography 행렬 미설정)일 경우
 *          transform()은 기본값(모든 필드 0)의 WorldPoint를 반환
 */
class ITrans {
public:
    virtual ~ITrans() = default;

    /**
     * @brief   정규화된 이미지 좌표 한 점을 Top-view 좌표로 변환
     * @param   x   정규화된 이미지 x 좌표 (-1.0 ~ 1.0, DetectedObject::cx 등)
     * @param   y   정규화된 이미지 y 좌표 (-1.0 ~ 1.0, DetectedObject::cy 등)
     * @return  변환된 Top-view 좌표 (WorldPoint). 캘리브레이션되지 않은 구현체의
     *          경우 기본값(x=0, y=0, distance=0)이 반환
     */
    virtual WorldPoint transform(double x, double y) const = 0;
};

/**
 * @brief   ITrans 구현체를 생성하는 팩토리 함수
 * @return  생성된 ITrans 구현체에 대한 shared_ptr
 */
std::shared_ptr<ITrans> createTrans();