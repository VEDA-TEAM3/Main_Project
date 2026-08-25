/**
 * @file veda_isr.h
 * @brief UART 수신·오류 콜백. HAL 이 콜백 하나로 모든 포트를 부르므로 여기서 회선을 가른다.
 *
 * HAL_UART_RxCpltCallback / HAL_UART_ErrorCallback 은 **약한 심볼 하나**뿐이라 포트별로
 * 나눠 둘 수 없다. 그래서 두 회선(USART6 하행 / USART1 ACK)의 분기만 이 모듈에 모으고,
 * 실제 처리는 각 모듈(veda_downlink / veda_rs485)에 맡긴다.
 *
 * ### 두 회선의 처리 방식이 다른 이유
 *
 * | 회선 | ISR 이 하는 일 | 이유 |
 * |---|---|---|
 * | USART6 (하행) | 큐에 넣기만 | 프레임이 27바이트라 조립까지 하면 오버런이 난다 |
 * | USART1 (ACK)  | 줄 조립까지 | 최대 11바이트라 한 바이트 시간 안에 끝나고, 기다리는 sched_task 가 바로 큐에서 꺼내야 한다 |
 *
 * ### 오류 콜백은 반드시 재무장해야 한다
 * 재무장하지 않으면 오류 한 번으로 수신이 영구히 멈춘다. STM32F4 USART에는 RX FIFO가 없어
 * 바이트 하나만 밀려도 오버런(ORE)이 나고, 그때 HAL이 RX 인터럽트를 끈다.
 *
 * ### 하행 상태머신은 오류 콜백에서 되돌리지 않는다
 * downlink_state 는 **rx_task 소유**라서 ISR이 건드리면 경합이 된다. 오류로 바이트가
 * 빠지면 체크섬이 깨져 어차피 버려지고, 회선이 조용해지면 rx_task의 DOWNLINK_RESYNC_MSEC
 * 타임아웃이 조각을 정리한다. 반면 ACK 줄은 ISR 소유라 여기서 직접 비워도 경합이 없다.
 */

#ifndef VEDA_ISR_H
#define VEDA_ISR_H

#include "main.h"

/**
 * @brief UART 수신 완료 인터럽트. 회선을 갈라 넘기고 즉시 재무장한다.
 * @details HAL 의 약한 심볼을 덮어쓴다. stm32f4xx_it.c 의 IRQ 핸들러 -> HAL_UART_IRQHandler
 * -> 이 함수 순으로 불린다.
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart);

/**
 * @brief UART 오류(오버런/프레이밍 등) 처리. 조각을 버리고 재무장한다.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart);

/**
 * @brief 하행(USART6) 수신을 연다. rx_task 가 루프에 들어가기 전에 한 번 부른다.
 * @details 수신 버퍼(rpi_rx_byte)가 이 모듈 소유라 재무장 함수도 여기에 둔다.
 * 스케줄러가 뜬 뒤에 부를 것 -- 미리 열면 큐가 아직 없는 상태에서 바이트가 도착한다.
 * 이후 재무장은 위의 두 콜백이 담당한다.
 */
void veda_downlink_rx_arm(void);

#endif /* VEDA_ISR_H */
