#pragma once

/**
 * @file DetectedObject.h
 * @brief 파이프라인 내부 전용 객체 표현
 *
 * @details
 * - @b parentId : ParentBasedRouter 가 BLUR/RISK 를 가르는 유일한 키.
 *      Head/LicensePlate 만 값을 갖는다.
 * - @b box : 이미지 평면 좌표(변환 전). TopViewObject.pos 는 호모그래피 변환 후의
 *      월드 좌표. 이 둘을 같은 타입으로 두면 IGroundPointExtractor/ICoordinateTransform
 *      이 무엇을 입력받고 무엇을 출력하는지 구분이 안 됨.
 */

#include <optional>

#include "Contract.h"
#include "domain/NormBox.h"

namespace domain {

/// @brief ONVIF ObjectId (채널 내부에서만 유일)
using ObjectId = veda::ObjectId;

/**
 * @struct DetectedObject
 * @brief 영상 분석 파이프라인에서 추출된 단일 감지 객체의 내부 상태
 */
struct DetectedObject {
    ObjectId id = 0;                                     ///< ONVIF ObjectId. 채널 내부에서만 유일.
    std::optional<ObjectId> parentId;                    ///< ONVIF Parent 속성. Head/LicensePlate 만 값을 가짐.
    veda::ObjectClass cls = veda::ObjectClass::Unknown;  ///< 분류 클래스

    /**
     * @brief ONVIF Likelihood.
     *
     * @note
     * - 의미없지만 일단 넣어둠
     */
    double likelihood = 0.0;

    NormBox box;  ///< 정규화 이미지 좌표. Transformation 적용 후.

    /**
     * @brief bbox 가 프레임 경계에 닿음 여부
     * @details 발이 잘렸을 수 있어 지면점 신뢰도가 낮다는 신호.
     *          IGroundPointExtractor 가 참고해 가중치를 낮출 수 있음.
     */
    bool touchesBorder = false;
};

}  // namespace domain