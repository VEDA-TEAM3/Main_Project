/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
/* USART1(RPi 링크) 1바이트 수신 인터럽트를 무장한다. rawRxByteQueue 가 만들어진 뒤에
 * 호출되어야 하므로 main()이 아니라 rx_task 진입부에서 부른다. */
void App_UartRxArm(void);
/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define B1_Pin GPIO_PIN_13
#define B1_GPIO_Port GPIOC
#define DEBUG_TX_Pin GPIO_PIN_2
#define DEBUG_TX_GPIO_Port GPIOA
#define DEBUG_RX_Pin GPIO_PIN_3
#define DEBUG_RX_GPIO_Port GPIOA
#define LD2_Pin GPIO_PIN_5
#define LD2_GPIO_Port GPIOA
#define CH2_LED_Pin GPIO_PIN_1
#define CH2_LED_GPIO_Port GPIOB
#define CH1_LED_Pin GPIO_PIN_2
#define CH1_LED_GPIO_Port GPIOB
#define CH3_LED_Pin GPIO_PIN_15
#define CH3_LED_GPIO_Port GPIOB
#define USART_TX_Pin GPIO_PIN_9
#define USART_TX_GPIO_Port GPIOA
#define USART_RX_Pin GPIO_PIN_10
#define USART_RX_GPIO_Port GPIOA
#define TMS_Pin GPIO_PIN_13
#define TMS_GPIO_Port GPIOA
#define TCK_Pin GPIO_PIN_14
#define TCK_GPIO_Port GPIOA
#define SWO_Pin GPIO_PIN_3
#define SWO_GPIO_Port GPIOB
#define SIREN_Pin GPIO_PIN_4
#define SIREN_GPIO_Port GPIOB
#define BUZZER_Pin GPIO_PIN_5
#define BUZZER_GPIO_Port GPIOB
#define CH0_LED_Pin GPIO_PIN_6
#define CH0_LED_GPIO_Port GPIOB
#define LED_YELLOW_Pin GPIO_PIN_7
#define LED_YELLOW_GPIO_Port GPIOB
#define LED_GREEN_Pin GPIO_PIN_8
#define LED_GREEN_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
/* 위 핀 정의는 전부 .ioc 에 등록되어 CubeMX 가 생성한다(예전에는 이 USER CODE 블록에
 * 손으로 적어두었는데, 그러면 .ioc 를 모르는 CubeMX 가 같은 핀을 다른 기능에 배정해도
 * 아무 경고 없이 통과해버린다).
 * TODO: SIREN(PB4)/BUZZER(PB5)/LED_YELLOW(PB7)/LED_GREEN(PB8) 은 아직 실제 배선이 아닌
 *       미사용 핀 placeholder -- 배선 확정되면 .ioc 에서 옮길 것.
 * 채널 LED 배선: CH0=PB6 노랑, CH1=PB2 초록, CH2=PB1 노랑, CH3=PB15 빨강 */
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
