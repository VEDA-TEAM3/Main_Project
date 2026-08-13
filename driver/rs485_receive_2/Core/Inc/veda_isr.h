/**
 * @file veda_isr.h
 * @brief USART1 수신·오류 콜백. HAL 이 콜백 하나로 모든 포트를 부르므로 여기서 갈라 준다.
 *
 * 이 보드에서 인터럽트 수신을 쓰는 포트는 USART1(RS-485) 하나뿐이다. USART2(로그)는
 * 송신 전용이라 콜백이 없다.
 *
 * ### 오류 콜백은 반드시 재무장해야 한다
 * 재무장하지 않으면 오류 한 번으로 수신이 영구히 멈춘다. 조립 중이던 프레임은 함께 버린다.
 */

#ifndef VEDA_ISR_H
#define VEDA_ISR_H

#include "main.h"

/**
 * @brief USART1 수신 완료 인터럽트. 한 바이트를 처리하고 즉시 재무장한다.
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart);

/**
 * @brief USART1 오류(오버런/프레이밍 등) 처리. 조립 중이던 프레임을 버리고 재무장한다.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart);

#endif /* VEDA_ISR_H */
