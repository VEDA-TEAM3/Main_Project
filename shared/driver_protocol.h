#pragma once

/**
 * @file driver_protocol.h
 * @brief rpi와 STM32 간의 UART 통신 규약
 * @note 순수 C언어 호환
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)

/**
 * @struct veda_hw_control_cmd_t
 * @brief STM32로 전송할 채널별 하드웨어 액추에이터 제어 명령
 */
typedef struct {
    uint8_t channel_id;  // 제어 대상 채널 (0~3)
    uint8_t siren_on;    // 경광등 (0: Off, 1: On)
    uint8_t buzzer_on;   // 부저 (0: Off, 1: On)
    uint8_t led_red;     // 빨강 LED 릴레이 (0: Off, 1: On)
    uint8_t led_yellow;  // 노랑 LED 릴레이 (0: Off, 1: On)
    uint8_t led_green;   // 초록 LED 릴레이 (0: Off, 1: On)
} veda_hw_control_cmd_t;

/**
 * @struct veda_uart_packet_t
 * @brief UART 프레이밍을 위한 전체 패킷 구조
 */
typedef struct {
    uint8_t start_byte;         // 통신 시작 바이트 (예: 0xST)
    veda_hw_control_cmd_t cmd;  // 제어 데이터 페이로드
    uint8_t checksum;           // 단순 XOR 체크섬
    uint8_t end_byte;           // 통신 종료 바이트 (예: 0xED)
} veda_uart_packet_t;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif