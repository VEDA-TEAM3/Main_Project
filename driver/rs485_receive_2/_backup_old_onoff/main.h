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

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define B1_Pin GPIO_PIN_13
#define B1_GPIO_Port GPIOC
#define RELAY_A_Pin GPIO_PIN_0
#define RELAY_A_GPIO_Port GPIOA
#define USART_TX_Pin GPIO_PIN_2
#define USART_TX_GPIO_Port GPIOA
#define USART_RX_Pin GPIO_PIN_3
#define USART_RX_GPIO_Port GPIOA
#define LD2_Pin GPIO_PIN_5
#define LD2_GPIO_Port GPIOA
#define RS485_RX_Pin GPIO_PIN_10
#define RS485_RX_GPIO_Port GPIOA
#define TMS_Pin GPIO_PIN_13
#define TMS_GPIO_Port GPIOA
#define TCK_Pin GPIO_PIN_14
#define TCK_GPIO_Port GPIOA
#define SWO_Pin GPIO_PIN_3
#define SWO_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
/*
 * 이 보드가 구동하는 하드웨어 핀. 배선 규격표(STM32 공통 핀맵)를 그대로 옮긴 것이다.
 *
 *   채널 A : PA0(A0) 경광등 릴레이 + PB5(D4) 부저 + PA6(D12) NeoPixel
 *   채널 B : PA1(A1) 경광등 릴레이 + PA8(D7) 부저 + PB10(D6) NeoPixel
 *
 * 규격표의 핀은 확정값이라 다른 기능에 겹쳐 쓰지 않는다. 특히 PA8은 예전에 NeoPixel DIN
 * 으로 잡혀 있었는데(NEOPIXEL_DIN), 규격상 부저 B 자리라 그 정의를 걷어냈다.
 *
 * RELAY_A 를 뺀 나머지는 .ioc 에 없는 핀이라 CubeMX 의 MX_GPIO_Init() 이 건드리지 않는다.
 * USER CODE 영역의 초기화 함수들이 직접 잡으므로 CubeMX 로 코드를 다시 생성해도 살아남는다.
 */

/**
 * 채널 B 경광등 릴레이. Nucleo-F401RE 기준 A1 이고 채널 A(PA0 = A0) 바로 옆이라
 * 두 경광등 배선을 같은 헤더에서 뽑을 수 있다.
 *
 * GPIO 가 릴레이 코일을 직접 물지 않는다. 330R 을 거쳐 2N7000 게이트를 흔들고,
 * MOSFET 이 릴레이 모듈 IN 을 GND 로 당긴다(Active-Low 릴레이 기준).
 * 게이트에는 10k 풀다운이 붙어 부팅 중 핀이 떠도 릴레이가 붙지 않는다.
 * 즉 코드 기준으로는 PA0/PA1 High = 경광등 ON 이다.
 */
#define RELAY_B_Pin GPIO_PIN_1
#define RELAY_B_GPIO_Port GPIOA

/**
 * 채널 A 능동 부저 모듈의 S 단자. Arduino 헤더의 D4 = PB5 다.
 *
 * 부저 모듈 전원은 5V 를 쓰고 GND 를 보드와 공통으로 묶는다. 모듈마다 Active-High /
 * Active-Low 가 갈리므로 실물로 확인하고 BUZZER_ACTIVE_HIGH 를 맞출 것.
 *
 * 인접한 D3(PB3)은 SWO 라 디버거와 겹치므로 쓰면 안 된다.
 */
#define BUZZER_A_Pin GPIO_PIN_5
#define BUZZER_A_GPIO_Port GPIOB

/**
 * 채널 B 능동 부저 모듈의 S 단자. Arduino 헤더의 D7 = PA8 이다.
 *
 * 예전에 이 핀이 NeoPixel DIN(NEOPIXEL_DIN)으로 잡혀 있었다. 규격표상 부저 B 자리이고
 * NeoPixel 은 PA6/PB10 으로 따로 배정되어 있어, 중복 배정을 없애고 부저로 되돌렸다.
 * .ioc 의 PA8 설정도 함께 지웠으므로 MX_GPIO_Init() 이 더 이상 이 핀을 건드리지 않는다.
 */
#define BUZZER_B_Pin GPIO_PIN_8
#define BUZZER_B_GPIO_Port GPIOA

/**
 * 채널 A NeoPixel(WS2812) 줄 조명의 DIN. Arduino 헤더의 D12 = PA6 다.
 *
 * 이 핀은 GPIO 출력이 아니라 TIM3_CH1(AF2) 의 PWM 출력으로 쓴다. WS2812 는 800kHz 로
 * 비트마다 High 폭을 달리해 0/1 을 구분하므로, 핀을 High 로 놓는다고 켜지지 않는다.
 * 실제 구동은 neopixel.c 가 전담한다(TIM3 + DMA1 Stream4 Channel5).
 *
 * 배선: PA6 -> 74AHCT125 -> 330R -> NeoPixel DIN (레벨 시프터 /OE 는 GND).
 */
#define NEOPIXEL_A_Pin GPIO_PIN_6
#define NEOPIXEL_A_GPIO_Port GPIOA

/**
 * 채널 B NeoPixel(WS2812) 줄 조명의 DIN. Arduino 헤더의 D6 = PB10 이다.
 *
 * A 채널과 달리 TIM2_CH3(AF1) + DMA1 Stream1 Channel3 를 쓴다. 핀만 바꿔 쓸 수 없는 이유는
 * 각 타이머 채널의 DMA 요청이 정해진 스트림 한 자리에만 걸리기 때문이다.
 *
 * 배선: PB10 -> 74AHCT125 -> 330R -> NeoPixel DIN (레벨 시프터 /OE 는 GND).
 */
#define NEOPIXEL_B_Pin GPIO_PIN_10
#define NEOPIXEL_B_GPIO_Port GPIOB
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
