#pragma once

/**
 * @file    DetectedObject.h
 * @brief   Pipeline에 사용될 내부용 단일 객체
 */

#include <optional>

#include "Contract.h"
#include "domain/NormBox.h"

namespace domain {

using ObjectId = veda::ObjectId;

/**
 * @brief Pipeline에 사용될 내부용 단일 객체
 */
struct DetectedObject {
    ObjectId id = 0;                                     ///< ObjectId (CCTV Channel에서만 유일)
    std::optional<ObjectId> parentId;                    ///< ParentId (Head/LicensePlate만 값을 가짐)
    veda::ObjectClass cls = veda::ObjectClass::Unknown;  ///< ObjectType
    NormBox box;                                         ///< 정규화 이미지 좌표
    bool touchesBorder = false;  ///< bbox가 프레임 경계(어느 변이든)에 닿음 여부

    /**
     * @brief   bbox 아래변이 프레임 하단 경계에 닿음 여부
     *
     * @details
     * touchesBorder 와 따로 두는 이유는 지면점(bbox 아래변 중앙)의 신뢰도가
     * '어느 변이 잘렸는가'에 따라 완전히 다르기 때문
     * - 좌/우/위 변이 잘림  : 발이 여전히 보이므로 지면점은 유효
     * - 아래변이 잘림        : 발 위치를 모르는 채 잘린 지점을 지면으로 오인
     *                        -> 호모그래피가 실제보다 훨씬 먼 곳으로 사상 (수 미터 오차)
     *
     * @note 판정은 파서(Metadata 좌표계)에서만 수행. 매퍼는 blur 경로 전용이 되었으므로
     *       더 이상 경계 판정을 다시 계산하지 않음
     */
    bool bottomTruncated = false;
};

}  // namespace domain