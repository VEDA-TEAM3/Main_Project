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
 *   MCU      : STM32F401RETx (NUCLEO-F401RE), SYSCLK 84MHz
 *   스트립   : SZH-LD314 (12V 5050, 800kHz 단선 데이터)
 *
 * [!! 12V 스트립의 '픽셀' 의미 !!]
 *   12V 로 도는 "WS2812" 스트립은 LED 안에 컨트롤러가 든 WS2812B 가 아니라, 외부 드라이버
 *   IC 인 WS2811 이 LED 3개를 묶어 구동한다. 프로토콜은 같은 800kHz 단선이지만 주소 단위가
 *   다르다 -- 이 드라이버가 말하는 '픽셀' 1개 = 실제 LED 3개이고, 그 3개는 항상 같은 색이다.
 *
 *   즉 LED 9개짜리 구간은 컨트롤러 3개다. 그런데 이 드라이버는 픽셀 9개를 보낸다:
 *   앞의 3개를 컨트롤러 3개가 가져가고 남는 6개분은 줄 끝을 지나 그냥 버려지므로 무해하고,
 *   반대로 개별 제어되는 5V 스트립(LED 9개 = 픽셀 9개)을 꽂아도 그대로 맞는다.
 *   적게 보내는 쪽이 위험하다 -- 뒤쪽이 갱신되지 않아 꺼진 채로 남는다.
 *
 * [확정 핀 배정 -- 바꿀 수 없다]
 *
 *   채널   GPIO   Arduino   타이머      AF    DMA (CC 요청)
 *   ----   ----   -------   ---------   ---   -----------------------
 *   A      PA6    D12       TIM3 CH1    AF2   DMA1 Stream4, Channel 5
 *   B      PA7    D11       TIM3 CH2    AF2   DMA1 Stream5, Channel 5
 *
 *   핀만 따로 옮길 수 없다. 각 타이머 채널의 DMA 요청은 정해진 스트림 한 자리에만
 *   걸리므로, 핀을 바꾸면 타이머와 DMA 스트림/채널이 한 묶음으로 따라온다.
 *
 *   두 채널이 TIM3 하나를 나눠 쓴다(CH1/CH2). 카운터가 하나뿐이라 동시에 송신할 수 없다 --
 *   아래 [스레드 안전성] 항목이 그래서 더 엄해진다.
 *
 * [배선]
 *   STM32(3.3V) --> 74AHCT125 --> 330R --> 스트립 DIN
 *   레벨 시프터의 그 채널 /OE 는 GND 로, VCC 는 5V 로. 쓰지 않는 입력은 띄우지 말 것.
 *   스트립 전원(12V)과 보드 GND 를 반드시 공통으로 묶는다. GND 가 안 묶이면 데이터
 *   기준점이 없어 아무것도 켜지지 않거나 앞쪽 몇 개만 엉뚱하게 켜진다.
 *   LED 전원은 12V 외부 전원에서 뽑는다 -- MCU 핀으로 공급하지 않는다.
 *
 * [동작 방식]
 *   타이머를 800kHz(비트당 1.25us)로 돌리고, 비트마다 CCR(High 폭)을 DMA 로 갈아끼운다.
 *   0 비트는 짧은 High(약 0.30us), 1 비트는 긴 High(약 0.80us)다.
 *   즉 이 핀들은 레벨이 아니라 '폭'으로 데이터를 싣는다 -- HAL_GPIO_WritePin() 으로
 *   High 를 줘 봐야 절대 켜지지 않는다.
 *
 * [스레드 안전성]
 *   neopixel_show_solid() 는 전송이 끝날 때까지 기다리는 블로킹 함수다(9픽셀 약 0.9ms).
 *   ISR 에서 부르면 그동안 다른 인터럽트가 밀리므로 태스크 문맥에서만 부를 것.
 *   두 채널이 PWM 인코딩 버퍼를 공유하므로, 여러 태스크에서 동시에 부르지 말 것.
 */

/** 줄 조명 채널. 값은 내부 하드웨어 표의 인덱스로 그대로 쓰인다. */
typedef enum
{
    NEOPIXEL_CH_A = 0,
    NEOPIXEL_CH_B = 1,
    NEOPIXEL_CH_COUNT = 2
} neopixel_channel_t;

/** 한 채널에 물릴 수 있는 최대 LED 수. DMA 버퍼가 이 수에 맞춰 잡혀 있다. */
#define NEOPIXEL_MAX_PIXEL_COUNT 9U

/** 실제로 달려 있는 LED 수. 응용 코드는 이 값을 그대로 넘기면 된다. */
#define NEOPIXEL_PIXEL_COUNT 9U

/**
 * @brief 지정한 채널의 타이머/DMA/GPIO를 NeoPixel 송신용으로 준비한다.
 *
 * @param channel 초기화할 채널
 *
 * @return HAL_OK 준비 완료
 * @return HAL_ERROR 채널 번호가 범위 밖이다
 *
 * @details 쓰는 채널만 부르면 된다. 부르지 않은 채널의 핀은 건드리지 않으므로,
 * A 만 쓰는 보드에서 PB10 을 다른 용도로 남겨 둘 수 있다.
 *
 * 첫 전송 전에 한 번 부를 것. 같은 채널에 두 번 불러도 안전하다(멱등).
 * 내부에서 HAL_Delay() 로 라인을 안정시키므로 태스크 문맥에서 부를 것.
 */
HAL_StatusTypeDef neopixel_init(neopixel_channel_t channel);

/**
 * @brief NeoPixel LED를 동일한 색상으로 출력한다.
 *
 * @param channel 출력할 채널
 * @param pixel_count 출력할 LED 개수 (1 ~ NEOPIXEL_MAX_PIXEL_COUNT)
 * @param red 빨강 밝기 (0~255)
 * @param green 초록 밝기 (0~255)
 * @param blue 파랑 밝기 (0~255)
 *
 * @return HAL_OK 전송 성공
 * @return HAL_ERROR 잘못된 인자 또는 DMA 오류
 * @return HAL_TIMEOUT DMA 전송 제한시간 초과
 *
 * @details 인자는 사람이 읽기 쉬운 RGB 순이지만, 회선에는 이 스트립이 받는 순서인 BRG 로
 * 나간다(각 바이트는 MSB first). WS2812B 규격의 GRB 가 아니다 -- 이 보드에 물린 12V
 * 스트립은 WS2811 이라 배선이 다르고, 근거는 구현부 주석에 실물 관찰표로 적어 두었다.
 * 태스크 문맥에서만 부를 것 -- 전송이 끝날 때까지 기다린다.
 */
HAL_StatusTypeDef neopixel_show_solid(neopixel_channel_t channel,
                                      uint16_t pixel_count,
                                      uint8_t red,
                                      uint8_t green,
                                      uint8_t blue);

#ifdef __cplusplus
}
#endif
