#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

/*
 * ============================================================================
 *  NeoPixel(WS2812 호환) 2채널 드라이버 -- NUCLEO-F401RE 전용
 * ============================================================================
 *
 * 이 파일이 다루는 것은 줄 조명 제어뿐이다. 통신/위험도/부저/경광등은 여기 없다.
 *
 * [대상 하드웨어]
 *   MCU      : STM32F401RETx (NUCLEO-F401RE)
 *   스트립   : SZH-LD314 (WS2812 호환 단선 데이터, 800kHz)
 *   스트립 전원 : 외부 12V
 *   최대 LED : 채널당 300
 *
 * [확정 핀 배정 -- 바꿀 수 없다]
 *
 *   채널   GPIO   Arduino   타이머      AF    DMA
 *   ----   ----   -------   ---------   ---   -----------------------
 *   A      PA6    D12       TIM3 CH1    AF2   DMA1 Stream4, Channel 5
 *   B      PB10   D6        TIM2 CH3    AF1   DMA1 Stream1, Channel 3
 *
 *   핀만 따로 옮길 수 없다. 각 타이머 채널의 DMA 요청은 정해진 스트림 한 자리에만
 *   걸리므로, 핀을 바꾸면 타이머와 DMA 스트림/채널이 한 묶음으로 따라온다.
 *
 * [배선]
 *   STM32(3.3V) --> 74AHCT125 --> 330R --> 스트립 DIN
 *   레벨 시프터의 그 채널 /OE 는 GND 로, VCC 는 5V 로. 쓰지 않는 입력은 띄우지 말 것.
 *   스트립 전원(12V)과 보드 GND 를 반드시 공통으로 묶는다. GND 가 안 묶이면 데이터
 *   기준점이 없어 아무것도 켜지지 않거나 앞쪽 몇 개만 엉뚱하게 켜진다.
 *
 * [!! 12V 스트립의 픽셀 수 !!]
 *   12V 로 도는 WS2812 호환 스트립은 대개 컨트롤러 IC 하나가 LED 3개를 묶어 구동한다.
 *   그래서 이 드라이버가 말하는 '픽셀' 1개 = 실제 LED 3개이고, 3개가 항상 같은 색으로
 *   함께 켜진다. 개별 제어가 되는 최소 단위가 3개짜리 덩어리다.
 *
 *   pixel_count 에는 LED 개수가 아니라 '컨트롤러 개수'(= LED 수 / 3)를 넣어야 한다.
 *   여기를 LED 개수로 넣으면 필요한 것의 3배를 보내게 되는데, 남는 데이터는 마지막
 *   컨트롤러를 지나 버려지므로 동작에는 문제가 없다(느려질 뿐이다).
 *   반대로 너무 적게 넣으면 그 뒤쪽이 갱신되지 않아 꺼진 채로 남는다.
 *
 * [동작 방식]
 *   타이머를 800kHz(비트당 1.25us)로 돌리고, 비트마다 CCR(High 폭)을 DMA 로 갈아끼운다.
 *   0 비트는 짧은 High(약 0.35us), 1 비트는 긴 High(약 0.70us)다.
 *   즉 이 핀들은 레벨이 아니라 '폭'으로 데이터를 싣는다 -- HAL_GPIO_WritePin() 으로
 *   High 를 줘 봐야 절대 켜지지 않는다.
 *
 * [스레드 안전성]
 *   송신 함수들은 전송이 끝날 때까지 기다리는 블로킹 함수다(300픽셀 약 9ms).
 *   ISR 에서 부르면 그동안 다른 인터럽트가 밀리므로 태스크 문맥에서만 부를 것.
 *   두 채널이 PWM 인코딩 버퍼를 공유하므로, 여러 태스크에서 동시에 부르지 말 것.
 */

/*
 * [비트 타이밍]
 *
 * 0/1 을 가르는 High 구간의 길이를 나노초로 적는다. 비트 하나의 전체 길이는 1.25us 로
 * 고정이고, 그 안에서 High 가 짧으면 0, 길면 1 이다.
 *
 * 기본값은 WS2812B 기준이다(T0H 0.35us / T1H 0.70us). 규격 허용 범위가 ±150ns 라
 * 대부분의 스트립이 이 값으로 동작한다.
 *
 * 색이 엉뚱하게 나오거나 켜졌다 말았다 하면 여기부터 조정할 것. 특히 12V 스트립은
 * WS2811 계열 컨트롤러를 쓰는 경우가 많은데, 그쪽은 T1H 를 더 길게(0.6~1.2us) 요구하는
 * 물건이 있다. 증상별로:
 *
 *   - 색이 전반적으로 어둡거나 파랑/초록으로 치우친다  -> T1H 를 늘려 본다 (700 -> 800)
 *   - 아무 색이나 흰색에 가깝게 나온다                 -> T0H 를 줄여 본다 (350 -> 250)
 *   - 앞쪽 몇 개만 맞고 뒤로 갈수록 흐트러진다         -> 타이밍보다 신호/전원 쪽을 볼 것
 *
 * 두 채널이 같은 값을 쓴다. 84MHz 기준으로 1틱 = 약 11.9ns 라 그보다 잘게는 못 나눈다.
 */
#ifndef NEOPIXEL_T0H_NS
#define NEOPIXEL_T0H_NS 350U
#endif
#ifndef NEOPIXEL_T1H_NS
#define NEOPIXEL_T1H_NS 700U
#endif

/** 줄 조명 채널. 값은 내부 표의 인덱스로 그대로 쓰인다. */
typedef enum
{
    NEOPIXEL_CH_A = 0,
    NEOPIXEL_CH_B = 1,
    NEOPIXEL_CH_COUNT = 2
} neopixel_channel_t;

/** 한 채널에 물릴 수 있는 최대 픽셀(컨트롤러) 수 */
#define NEOPIXEL_MAX_PIXEL_COUNT 300U

/**
 * @brief 지정한 채널의 타이머/DMA/GPIO를 송신용으로 준비하고 색 버퍼를 비운다.
 * @param channel 초기화할 채널
 * @retval HAL_OK 준비 완료
 * @retval HAL_ERROR 채널 번호가 범위 밖이다
 * @details 쓰는 채널만 부르면 된다. 부르지 않은 채널의 핀은 건드리지 않으므로,
 * A 만 쓰는 보드에서 PB10 을 다른 용도로 남겨 둘 수 있다.
 *
 * 태스크 문맥에서 부를 것 -- 내부에서 HAL_Delay() 로 라인을 안정시킨다.
 * 같은 채널에 두 번 불러도 안전하다(멱등).
 */
HAL_StatusTypeDef neopixel_init(neopixel_channel_t channel);

/**
 * @brief 색 버퍼의 픽셀 하나를 정한다. 회선에는 나가지 않는다.
 * @param channel 대상 채널
 * @param index 0-based 픽셀 번호
 * @param red 빨강 밝기(0~255)
 * @param green 초록 밝기(0~255)
 * @param blue 파랑 밝기(0~255)
 * @retval HAL_ERROR 채널 또는 index 가 범위 밖이다
 * @details 여러 개를 찍은 뒤 neopixel_show() 를 한 번 부르는 것이 쓰는 방법이다.
 * 픽셀마다 송신하면 매번 리셋 구간이 끼어 눈에 띄게 느려진다.
 */
HAL_StatusTypeDef neopixel_set_pixel(neopixel_channel_t channel, uint16_t index,
                                     uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief 색 버퍼 앞쪽 count 개를 같은 색으로 채운다. 회선에는 나가지 않는다.
 * @param channel 대상 채널
 * @param count 채울 픽셀 수 (1 ~ NEOPIXEL_MAX_PIXEL_COUNT)
 * @retval HAL_ERROR 인자가 범위 밖이다
 */
HAL_StatusTypeDef neopixel_fill(neopixel_channel_t channel, uint16_t count,
                                uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief 색 버퍼를 통째로 지우고(전부 소등) 그것을 회선에 내보낸다.
 * @param channel 대상 채널
 * @param count 소등할 픽셀 수
 * @retval 송신 결과. neopixel_show() 와 같다.
 */
HAL_StatusTypeDef neopixel_clear(neopixel_channel_t channel, uint16_t count);

/**
 * @brief 색 버퍼의 앞쪽 count 개를 실제로 스트립에 내보낸다.
 * @param channel 대상 채널
 * @param count 내보낼 픽셀 수 (1 ~ NEOPIXEL_MAX_PIXEL_COUNT)
 * @retval HAL_OK 전부 나갔다
 * @retval HAL_ERROR 인자가 범위 밖이거나 DMA 오류가 났다
 * @retval HAL_TIMEOUT 제한 시간 안에 전송이 끝나지 않았다
 * @details 태스크 문맥에서만 부를 것 -- 전송이 끝날 때까지 기다린다(300픽셀 약 9ms).
 *
 * HAL_OK 인데도 줄 끝이 어두우면 데이터는 전부 나간 것이므로 코드가 아니라 신호
 * (레벨 시프터)나 전원 쪽 문제다. 스트립이 길면 앞쪽에서 전압이 떨어져 뒤쪽이 어두워지거나
 * 색이 틀어지는데, 그때는 줄 끝에도 전원을 따로 물려야 한다.
 */
HAL_StatusTypeDef neopixel_show(neopixel_channel_t channel, uint16_t count);

/**
 * @brief 앞쪽 count 개를 한 색으로 칠하고 곧바로 내보낸다.
 * @details neopixel_fill() + neopixel_show() 를 한 번에 하는 편의 함수다.
 * 인자는 RGB 순이지만 회선에는 GRB 순으로 나간다(WS2812 규격).
 */
HAL_StatusTypeDef neopixel_show_solid(neopixel_channel_t channel, uint16_t count,
                                      uint8_t red, uint8_t green, uint8_t blue);

#ifdef __cplusplus
}
#endif
