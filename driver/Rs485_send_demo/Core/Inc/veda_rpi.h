/**
 * @file veda_rpi.h
 * @brief 관제 서버(Raspberry Pi)와 연결된 USART6 회선 자체. 상행과 하행이 이 포트를 공유한다.
 *
 * 이 모듈이 갖는 것은 '회선'뿐이다 -- 무엇을 보내고 받는지는 veda_uplink / veda_downlink 가
 * 정한다. 둘로 갈라 둔 이유는 huart6 의 소유자를 하나로 두기 위해서다.
 *
 * ### 배선
 * ```
 * PC6 (USART6_TX) -> RPi RXD (GPIO15)
 * PC7 (USART6_RX) <- RPi TXD (GPIO14)
 * GND 공통
 * ```
 * 다른 핀을 쓰려면 PA11(TX)/PA12(RX)도 같은 AF8로 USART6에 연결된다.
 *
 * ### 왜 직접 초기화하는가
 * USART6는 .ioc에 없어서 CubeMX가 코드를 만들어 주지 않는다. 클럭·GPIO·NVIC까지 이 모듈이
 * 직접 처리한다. main.c 쪽 호출도 USER CODE 영역에 있어 CubeMX로 코드를 다시 생성해도
 * 지워지지 않는다.
 */

#ifndef VEDA_RPI_H
#define VEDA_RPI_H

#include <stdint.h>

#include "main.h"

/** 관제 서버(Raspberry Pi)와 연결된 UART. .ioc에 없으므로 이 모듈이 직접 초기화한다. */
extern UART_HandleTypeDef huart6;

/**
 * @brief USART6를 초기화한다(클럭·GPIO·UART·NVIC). MX_*_Init 들 뒤, 스케줄러 시작 전에 부를 것.
 * @details 인터럽트 우선순위 5는 FreeRTOS의 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 와
 * 같은 값이다. 이보다 높은(숫자가 작은) 우선순위를 주면 ISR 안에서 RTOS API 를 부를 수 없다.
 */
void veda_rpi_uart_init(void);

#endif /* VEDA_RPI_H */
