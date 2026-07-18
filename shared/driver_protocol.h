#pragma once

/**
 * @file    driver_protocol.h
 * @brief   rpi와 STM32 간의 UART 통신 규약
 *
 * @note
 * - 순수 C언어 호환
 * RPi(C++)와 STM32(C) 양쪽이 이 헤더를 그대로 include해서 veda_checksum() 을 공유
 * -- 체크섬 알고리즘이 두 구현으로 갈라지는 걸 원천 차단
 *
 * @note
 * - 모든 구조체는 int64_t 필드 앞에 수동 reserved 를 넣어 8바이트 경계에 정렬시키고,
 * 구조체 전체 크기도 8의 배수로 맞춤
 * #pragma pack(1) 로 컴파일러 자동 패딩을 끄되, 정렬은 이 reserved 필드로 직접 보장
 * -- 컴파일러/아키텍처에 따라 달라지는 암묵적 패딩에 의존하지 않기 위함
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)

/* PLACEHOLDER */
#define VEDA_START_BYTE 0x53 /* 'S' */
#define VEDA_END_BYTE 0x45   /* 'E' */

/** dist_mm 에 유효한 거리 값이 없음을 나타내는 sentinel (0은 0mm라는 유효값이라 사용 불가) */
#define VEDA_DIST_MM_NONE 0xFFFFu

typedef enum { VEDA_RISK_NONE = 0, VEDA_RISK_WARNING = 1, VEDA_RISK_DANGER = 2 } veda_risk_level_t;

typedef enum { VEDA_UPLINK_REASON_ACK = 0, VEDA_UPLINK_REASON_HEARTBEAT = 1 } veda_uplink_reason_t;

/**
 * @brief RPi → STM32 하행: 위험 이벤트 통지 (총 24바이트, 8의 배수)
 */
typedef struct {
    uint8_t channel_id;   /* offset 0 */
    uint8_t risk_level;   /* offset 1 */
    uint8_t reserved0[6]; /* offset 2-7: 항상 0으로 채울것 */
    int64_t timestamp_ms; /* offset 8-15 */
    uint16_t dist_mm;     /* offset 16-17. 값 없으면 VEDA_DIST_MM_NONE */
    uint8_t reserved1[6]; /* offset 18-23: 항상 0으로 채울것 */
} veda_risk_event_t;      /* sizeof == 24 */

/**
 * @brief STM32 → RPi 상행: ACK/HEARTBEAT 통합 (총 16바이트, 8의 배수)
 */
typedef struct {
    uint8_t channel_id;   /* offset 0 */
    uint8_t reason;       /* offset 1: veda_uplink_reason_t */
    uint8_t siren_on;     /* offset 2 */
    uint8_t buzzer_on;    /* offset 3 */
    uint8_t led_red;      /* offset 4 */
    uint8_t led_yellow;   /* offset 5 */
    uint8_t led_green;    /* offset 6 */
    uint8_t reserved0[1]; /* offset 7: 항상 0으로 채울것 */
    int64_t timestamp_ms; /* offset 8-15 */
} veda_uplink_packet_t;   /* sizeof == 16 */

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
 */
static inline uint8_t veda_checksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
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

#pragma pack(pop)

#ifdef __cplusplus
}
#endif