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
    bool touchesBorder = false;                          ///< bbox가 프레임 경계에 닿음 여부
};

}  // namespace domain