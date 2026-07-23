#pragma once

/**
 * @file    ICoordinateTransform.h
 * @brief   정규화 이미지 좌표를 '카메라 로컬' 지면 좌표로 변환하는 인터페이스
 *
 * @warning [ 월드 좌표가 아님 ]
 * 연산 서버는 도면을 전혀 모른다. 여기서 나오는 좌표의 원점은 카메라이고 +y 는 카메라
 * 전방이다 (veda::LocalPoint 참고)
 * 도면 공통 좌표로 옮기는 것은 control-server 의 ILocalToWorldTransform 책임
 * -- 예전에는 이 함수 이름이 toWorld() 이고 반환 타입도 veda::WorldPoint 여서
 *    "여기서 이미 월드 좌표가 나온다"고 오해하기 쉬웠음. 그 상태로 호모그래피를
 *    도면 좌표에 맞춰 캘리브레이션하면 control-server 가 한 번 더 회전시켜
 *    예외도 경고도 없이 완전히 틀린 위치가 됨
 */

#include <optional>

#include "Contract.h"
#include "interfaces/IGroundPointExtractor.h"

/**
 * @brief 정규화 이미지 좌표를 카메라 로컬 지면 좌표(m)로 사상하는 인터페이스
 */
class ICoordinateTransform {
public:
    virtual ~ICoordinateTransform() = default;

    /**
     * @brief   2D 이미지 평면 상의 지면점을 카메라 로컬 지면 좌표로 사상
     *
     * @param   p 변환할 정규화 이미지 좌표
     * @return  카메라 로컬 좌표 (m). 지평선 너머 등 사상 불가면 nullopt
     */
    virtual std::optional<veda::LocalPoint> toLocal(const domain::ImagePoint& p) = 0;
};