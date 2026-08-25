#pragma once

/**
 * @file    driver_protocol.h
 * @brief   rpi와 STM32 간의 UART 통신 규약
 *
 * @note
 * - 순수 C언어 호환
 * RPi(C++)와 STM32(C) 양쪽이 이 헤더를 그대로 include해서 veda_checksum()을 공유
 * -- 체크섬 알고리즘이 두 구현으로 갈라지는 걸 원천 차단
 *
 * @note
 * - #pragma pack(1)로 wire layout을 고정하므로 다중 바이트 필드는 정렬되지 않을 수 있음
 * - timestamp_ms와 dist_mm은 직접 읽고 쓰지 말고 veda_read/write_*_le()을 사용할 것
 * - 다중 바이트 필드의 wire byte order는 little-endian
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)

#define VEDA_START_BYTE 0x53 /* 'S' */
#define VEDA_END_BYTE 0x45   /* 'E' */

/** dist_mm에 유효한 거리 값이 없음을 나타내는 sentinel (0은 0mm라는 유효값이라 사용 불가) */
#define VEDA_DIST_MM_NONE 0xFFFFu

typedef enum { VEDA_RISK_NONE = 0, VEDA_RISK_WARNING = 1, VEDA_RISK_DANGER = 2 } veda_risk_level_t;

typedef enum { VEDA_UPLINK_REASON_ACK = 0, VEDA_UPLINK_REASON_HEARTBEAT = 1 } veda_uplink_reason_t;

/**
 * @brief RPi → STM32 하행: 위험 이벤트 통지 (총 24바이트, 8의 배수)
 */
typedef struct {
    uint8_t channel_id;    ///< offset 0
    uint8_t risk_level;    ///< offset 1
    uint8_t reserved0[6];  ///< offset 2-7 (항상 0으로 채울것)
    int64_t timestamp_ms;  ///< offset 8-15
    uint16_t dist_mm;      ///< offset 16-17 (값 없으면 VEDA_DIST_MM_NONE)
    uint8_t reserved1[6];  ///< offset 18-23 (항상 0으로 채울것)
} veda_risk_event_t;       ///< sizeof == 24

/**
 * @brief STM32 → RPi 상행: ACK/HEARTBEAT 통합 (총 16바이트, 8의 배수)
 */
typedef struct {
    uint8_t channel_id;    ///< offset 0
    uint8_t reason;        ///< offset 1: veda_uplink_reason_t
    uint8_t siren_on;      ///< offset 2
    uint8_t buzzer_on;     ///< offset 3
    uint8_t led_red;       ///< offset 4
    uint8_t led_yellow;    ///< offset 5
    uint8_t led_green;     ///< offset 6
    uint8_t reserved0[1];  ///< offset 7 (항상 0으로 채울것)
    int64_t timestamp_ms;  ///< offset 8-15
} veda_uplink_packet_t;    ///< sizeof == 16

typedef struct {
    uint8_t start_byte;
    veda_risk_event_t payload;
    uint8_t checksum;
    uint8_t end_byte;
} veda_downlink_frame_t;

typedef struct {
    uint8_t start_byte;
    veda_uplink_packet_t payload;
    uint8_t checksum;
    uint8_t end_byte;
} veda_uplink_frame_t;

/**
 * @brief   바이트 배열에 대한 단순 XOR 체크섬 계산
 * @details RPi/STM32 양쪽이 반드시 이 함수만 사용할 것 (각자 재구현 금지)
 * @warning 전송 오류 검출용이며 메시지 인증이나 위변조 방지 기능은 없음
 */
static inline uint8_t veda_checksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    if (data == NULL && len != 0) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        sum ^= data[i];
    }
    return sum;
}

static inline uint8_t veda_downlink_checksum(const veda_risk_event_t* payload) {
    return veda_checksum((const uint8_t*)payload, sizeof(*payload));
}

static inline uint8_t veda_uplink_checksum(const veda_uplink_packet_t* payload) {
    return veda_checksum((const uint8_t*)payload, sizeof(*payload));
}

/**
 * @brief Packed 구조체의 다중 바이트 필드를 little-endian으로 안전하게 읽고 쓰는 함수
 */
static inline void veda_write_u16_le(void* destination, uint16_t value) {
    uint8_t* bytes = (uint8_t*)destination;
    if (bytes == NULL) {
        return;
    }
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static inline uint16_t veda_read_u16_le(const void* source) {
    const uint8_t* bytes = (const uint8_t*)source;
    if (bytes == NULL) {
        return 0;
    }
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static inline void veda_write_i64_le(void* destination, int64_t value) {
    uint64_t bits = 0;
    uint8_t* bytes = (uint8_t*)destination;
    if (bytes == NULL) {
        return;
    }
    memcpy(&bits, &value, sizeof(bits));
    for (size_t i = 0; i < sizeof(bits); ++i) {
        bytes[i] = (uint8_t)(bits >> (i * 8));
    }
}

static inline int64_t veda_read_i64_le(const void* source) {
    const uint8_t* bytes = (const uint8_t*)source;
    uint64_t bits = 0;
    int64_t value = 0;
    if (bytes == NULL) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(bits); ++i) {
        bits |= (uint64_t)bytes[i] << (i * 8);
    }
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline int veda_uplink_payload_is_valid(const veda_uplink_packet_t* payload) {
    if (payload == NULL ||
        (payload->reason != VEDA_UPLINK_REASON_ACK && payload->reason != VEDA_UPLINK_REASON_HEARTBEAT)) {
        return 0;
    }
    if (payload->siren_on > 1 || payload->buzzer_on > 1 || payload->led_red > 1 || payload->led_yellow > 1 ||
        payload->led_green > 1) {
        return 0;
    }
    return payload->reserved0[0] == 0;
}

#if defined(__cplusplus)
static_assert(sizeof(veda_risk_event_t) == 24, "veda_risk_event_t wire ABI changed");
static_assert(sizeof(veda_uplink_packet_t) == 16, "veda_uplink_packet_t wire ABI changed");
static_assert(sizeof(veda_downlink_frame_t) == 27, "veda_downlink_frame_t wire ABI changed");
static_assert(sizeof(veda_uplink_frame_t) == 19, "veda_uplink_frame_t wire ABI changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(veda_risk_event_t) == 24, "veda_risk_event_t wire ABI changed");
_Static_assert(sizeof(veda_uplink_packet_t) == 16, "veda_uplink_packet_t wire ABI changed");
_Static_assert(sizeof(veda_downlink_frame_t) == 27, "veda_downlink_frame_t wire ABI changed");
_Static_assert(sizeof(veda_uplink_frame_t) == 19, "veda_uplink_frame_t wire ABI changed");
#endif

#pragma pack(pop)

#ifdef __cplusplus
}
#endif