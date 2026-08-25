#pragma once

/**
 * @file    SerialEventEncoding.h
 * @brief   UART 위험 이벤트의 채널/거리 필드를 프로토콜 범위 안으로 안전하게 변환
 */

#include <cmath>
#include <cstdint>
#include <limits>

#include "driver_protocol.h"

namespace serial_event {

static_assert(VEDA_DIST_MM_NONE == std::numeric_limits<std::uint16_t>::max());

constexpr std::uint16_t kMaxValidDistanceMm = static_cast<std::uint16_t>(VEDA_DIST_MM_NONE - 1u);
constexpr double kFirstReservedDistanceMeters = static_cast<double>(VEDA_DIST_MM_NONE) / 1000.0;

/**
 * @brief UART channel_id 필드로 표현 가능한 채널인지 검사
 */
inline bool isValidChannelId(int channelId) noexcept {
    return channelId >= 0 && channelId <= static_cast<int>(std::numeric_limits<std::uint8_t>::max());
}

/**
 * @brief 미터 단위 거리를 UART dist_mm 값으로 변환
 * @details 음수/NaN/무한대는 값 없음 sentinel로 바꾸고, sentinel과 충돌하는 거리부터는
 *          최대 유효값(65534mm)으로 포화한다. 범위 확인을 곱셈보다 먼저 수행해
 *          부동소수점→정수 범위 초과 변환의 정의되지 않은 동작을 피한다.
 */
inline std::uint16_t encodeDistanceMm(double distanceMeters) noexcept {
    if (!std::isfinite(distanceMeters) || distanceMeters < 0.0) {
        return VEDA_DIST_MM_NONE;
    }
    if (distanceMeters >= kFirstReservedDistanceMeters) {
        return kMaxValidDistanceMm;
    }
    const double distanceMm = distanceMeters * 1000.0;
    if (distanceMm >= static_cast<double>(VEDA_DIST_MM_NONE)) {
        return kMaxValidDistanceMm;
    }
    return static_cast<std::uint16_t>(distanceMm);
}

}  // namespace serial_event
