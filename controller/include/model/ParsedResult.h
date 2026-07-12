/**
 * @file    ParsedResult.h
 * @brief   IParser::parse()의 반환 타입.
 *          MetadataPacket 하나를 파싱한 결과가 TrackedPoint인지 BlurTarget인지 표현한다.
 * @note    한 MetadataPacket은 객체 하나(Human/Vehicle 또는 Face/LicensePlate)를 나타내므로
 *          trackedPoint와 blurTarget 중 정확히 하나만 값이 채워진다.
 */
#pragma once

#include <optional>

#include "model/TrackedPoint.h"
#include "model/outgoing/BlurTarget.h"

/**
 * @brief Parse된 결과값
 */
struct ParsedResult {
    std::optional<TrackedPoint> trackedPoint; /**< Human/Vehicle인 경우에만 값 존재 */
    std::optional<BlurTarget> blurTarget;     /**< Face/LicensePlate인 경우에만 값 존재 */
};