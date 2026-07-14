#pragma once

/**
 * @file NormBox.h
 * @brief 파이프라인 내부 처리용 정규화 Bounding Box 타입 별칭
 */

#include "Contract.h"

namespace domain {

/**
 * @brief 내부 파이프라인용 BBox 타입.
 * @details 외부 통신용 Contract 스키마(veda::NormRect)를 현재는 그대로 차용.
 */
using NormBox = veda::NormRect;

}  // namespace domain