#include "neopixel.h"

#include "main.h"

#include <string.h>

/*
 * NeoPixel(WS2812 호환) 2채널 드라이버 구현.
 * 배선/핀 배정은 neopixel.h 의 머리말에 정리되어 있다.
 *
 * 이 파일은 TIM3, DMA1 Stream4, DMA1 Stream5 를 단독으로 소유한다.
 * CubeMX 가 만든 TIM/DMA 핸들은 쓰지 않는다 -- 비트 단위로 CCR 을 갈아끼워야 해서
 * HAL 의 PWM API 로는 표현할 수 없기 때문이다.
 *
 * !! 두 채널이 TIM3 하나의 서로 다른 채널(CH1/CH2)을 쓴다. 카운터가 하나뿐이라 동시에
 *    송신할 수 없다 -- neopixel_show_solid() 가 블로킹이고 태스크 문맥에서만 불려
 *    전송이 겹치지 않는다는 전제 위에서만 성립한다. 쉬는 채널은 CCR 이 0 이라 그동안
 *    핀이 Low 로 남으므로(PWM 모드 1), 한쪽을 쏘는 것이 다른 쪽 줄을 흔들지 않는다.
 *
 * ---------------------------------------------------------------------------
 * [비트 타이밍]  타이머 클럭 84MHz, PSC = 0, ARR = 104
 *
 *   한 비트 = (ARR + 1) = 105 tick = 105 / 84MHz = 1.25us  (= 800kHz)
 *   논리 0  : CCR = 29 tick = 29 / 84MHz  = 0.3452us
 *   논리 1  : CCR = 58 tick = 58 / 84MHz  = 0.6905us
 *
 * 84MHz 는 이 보드의 SystemClock_Config() 에서 나온다. TIM3 은 APB1 에 달려 있고
 * APB1 분주가 2 라 타이머 클럭은 PCLK1(42MHz)의 2배가 된다. 두 채널이 아예 같은 타이머라
 * 상수도 하나로 공유한다. 클럭 설정을 바꾸면 위 세 상수를 다시 계산해야 한다.
 *
 * ---------------------------------------------------------------------------
 * [DMA 버퍼 구조]  uint16_t x 696
 *
 *   index          내용                    CCR 값
 *   ------------   ---------------------   -------------
 *   0   ~ 239      PRE RESET LOW (300us)   0
 *   240 ~ 455      9 LED 픽셀 데이터        29 또는 58
 *   456 ~ 695      POST RESET LOW (300us)  0
 *
 * 앞쪽 LOW 240 슬롯이 이 드라이버의 핵심이다. 전송을 시작하는 순간의 첫 PWM 펄스는
 * 타이머/DMA 가 물리는 시점 차이 때문에 폭이 어긋날 수 있는데, 그 자리에 실제 픽셀 데이터의
 * 첫 비트가 놓여 있으면 1번 LED 의 색이 틀어진다. 앞에 300us LOW 를 깔아 두면 그
 * 불확실한 구간이 전부 리셋 구간 안에서 소모되고, 실제 데이터는 안정된 뒤에 나간다.
 * 뒤쪽 LOW 는 스트립이 프레임 끝(리셋)을 인식해 받은 색을 반영하게 하는 용도다.
 *
 * ---------------------------------------------------------------------------
 * [DMA 요청은 CC 요청(CCxDE)이다]
 *
 * F401 의 요청 배선은 고정이다(RM0368 Table 27).
 *   TIM3_CH1 -> DMA1 Stream4 Channel5      TIM3_UP -> DMA1 Stream2 Channel5
 *   TIM3_CH2 -> DMA1 Stream5 Channel5      TIM3_CH3 -> DMA1 Stream7 Channel5
 *
 * 앞뒤 리셋 구간의 슬롯 값이 CCR = 0 이라, "CCR = 0 에서도 CC 매치가 잡히는가"가 이 구조의
 * 전제가 된다. 이 전제는 이 보드에서 실물로 확인되었다 -- 앞의 240 슬롯이 전부 0 인 채로
 * 전송이 끝까지 진행되고 색이 정상으로 나온다. 요청이 끊긴다면 데이터가 한 바이트도 나가지
 * 못해 LED 가 아예 켜지지 않으므로, 켜진다는 사실 자체가 증거다.
 *
 * 업데이트(UDE) 요청으로 바꾸면 CCR 값과 무관해져 이론상 더 안전하지만, TIM3_UP 요청은
 * Stream2 하나뿐이라 두 채널이 그 자리를 두고 부딪힌다(같은 타이머의 UP 이벤트라 어느
 * 채널을 쏘는지 구분되지도 않는다). 실물에서 확인된 구성을 이론을 근거로 바꾸지 않는다.
 * (다른 패밀리에서는 CCR = 0 에서 CC 요청이 멎는다는 보고가 있다. 나중에 F4 가 아닌 칩으로
 *  옮길 일이 생기면 TIM_DIER_UDE + Stream2 가 먼저 확인할 대안이다.)
 *
 * CCR 프리로드(OCxPE)는 반드시 켜 둔다. 끄면 DMA 가 주기 도중에 듀티를 바꿔 그 비트가
 * 깨진다. 프리로드 때문에 DMA 가 쓴 값은 다음 업데이트에서 활성 레지스터로 넘어간다.
 *
 * ---------------------------------------------------------------------------
 * [실제 출력 구간]  프리로드 지연까지 세어 회선에 나가는 파형을 정확히 적으면
 *
 *   주기 1          CCR = 0 (시작값)                 LOW
 *   주기 2 ~ 241    슬롯 0~239                       LOW   241 x 1.25us = 301.3us
 *   주기 242 ~ 457  슬롯 240~455 (9 LED 픽셀)         DATA  216 x 1.25us = 270.0us
 *   주기 458 ~ 696  슬롯 456~694                     LOW   239 x 1.25us = 298.8us
 *
 * 마지막 슬롯(695)은 TC 직후 타이머를 세우므로 회선에 나가지 않는다. 값이 0 이라 잃는
 * 것이 없고, 뒤 리셋도 298.8us 로 280us 요건을 넘는다. 그 뒤 타이머를 세운 상태에서
 * CCR = 0 이므로 핀은 계속 LOW 이고, 호출자에게 돌아가기 전 HAL_Delay(1) 이 1ms 를 더 준다.
 */

/* ---------------------------------------------------------------------------
 * [비트 폭]  1 tick = 1 / 84MHz = 11.905ns,  한 비트 = 105 tick = 1.25us
 *
 *   T0H 29 tick = 345.2ns   T0L 76 tick = 904.8ns
 *   T1H 58 tick = 690.5ns   T1L 47 tick = 559.5ns
 *
 * 이 값은 WS2811 / WS2812 / WS2812B 세 규격을 동시에 만족하는 교집합이다. 예전에는
 * 25 / 67 (297.6 / 797.6ns) 이었는데, 그 조합은 이 보드에 물린 12V 스트립이 쓰는
 * WS2811 규격을 두 군데서 벗어난다:
 *
 *   T1H 797.6ns  ->  WS2811 상한 750ns 를 47.6ns 초과
 *   T1L 452.4ns  ->  WS2811 하한 500ns 에 47.6ns 미달   <-- 이쪽이 실제로 위험하다
 *
 * 예전 주석은 T1H 초과만 적어 두고 "High 를 임계값과 비교하니 괜찮다"고 넘겼는데,
 * 빠진 것이 T1L 이다. High 가 길어진 만큼 그 비트의 Low 가 짧아지고, Low 가 규격보다
 * 짧으면 칩이 비트 경계를 놓쳐 스트림이 밀릴 수 있다. High 폭만 보면 이 경로가 보이지 않는다.
 *
 * !! 이 값을 고친 것이 색 문제를 고친 것은 아니다. 당시 색이 어긋난 원인은 따로 있었고
 *    (스트립이 GRB 가 아니라 BRG 로 받는다 -- neopixel_show_solid() 주석 참고), 25/67 로도
 *    비트가 실제로 밀린 정황은 없었다. 즉 여기 수정은 관측된 고장을 고친 것이 아니라
 *    규격 밖에서 우연히 동작하던 상태를 규격 안으로 되돌린 것이다. 잠재 위험만 제거했다.
 *    색이 어긋나면 타이밍보다 바이트 순서를 먼저 볼 것.
 *
 * [공식 규격표 -- 800kHz 모드, 모두 +-150ns 허용]
 *
 *   칩              T0H            T1H            T0L            T1L
 *   -------------   ------------   ------------   ------------   ------------
 *   WS2811          0.10~0.40us    0.45~0.75us    0.85~1.15us    0.50~0.80us
 *   WS2812          0.20~0.50us    0.55~0.85us    0.65~0.95us    0.45~0.75us
 *   WS2812B         0.25~0.55us    0.65~0.95us    0.70~1.00us    0.30~0.60us
 *   -------------   ------------   ------------   ------------   ------------
 *   교집합          0.25~0.40us    0.65~0.75us    0.85~0.95us    0.50~0.60us
 *   현재 값         0.3452us  OK   0.6905us  OK   0.9048us  OK   0.5595us  OK
 *
 * 네 칸 모두 교집합 한가운데에 있다. 아래 _Static_assert 가 이 표를 그대로 검사하므로,
 * ARR 이나 T0H/T1H 를 건드려 규격을 벗어나면 빌드가 선다 (예전 값 67 은 실제로 걸린다).
 *
 * [HSI 오차까지 본 여유]
 *   이 보드는 외부 크리스털 없이 HSI 로 돈다. HSI 는 상온 +-1%, 전 온도 범위 +-4% 다.
 *   최악(+4%)에서도 T1H 690.5 -> 718ns (상한 750 이내), T1L 559.5 -> 537ns (하한 500
 *   이상)이라 규격을 유지한다. 예전 값 67 은 상온 +-1% 만으로도 이미 상한 밖이었다.
 *
 * 리셋(latch) 시간은 칩마다 다르다. WS2811/구형 WS2812B 는 50us 이상, WS2812B-V5 와
 * WS2815 는 280us 이상을 요구한다. 이 구현은 앞 301.3us / 뒤 298.8us 라 가장 엄한 쪽도
 * 만족한다(계산 근거는 위 [실제 출력 구간] 참고).
 * ------------------------------------------------------------------------ */

/** 한 픽셀은 3바이트 = 24비트. 바이트 순서는 neopixel_show_solid() 주석 참고 */
#define WS2812_BITS_PER_PIXEL 24U
/** 데이터 앞뒤에 두는 Low 슬롯 수. 240 x 1.25us = 300us 로 규격의 리셋 시간을 넘긴다. */
#ifndef WS2812_RESET_SLOT_COUNT
#define WS2812_RESET_SLOT_COUNT 240U
#endif
/** 한 비트의 길이. (ARR + 1) = 105 tick = 1.25us @84MHz */
#define WS2812_TIMER_ARR 104U
/** 논리 0 의 High 폭 (tick). 29 / 84MHz = 345.2ns -- 세 규격 교집합 */
#ifndef WS2812_T0H_TICKS
#define WS2812_T0H_TICKS 29U
#endif
/** 논리 1 의 High 폭 (tick). 58 / 84MHz = 690.5ns -- 세 규격 교집합 */
#ifndef WS2812_T1H_TICKS
#define WS2812_T1H_TICKS 58U
#endif
/** 전송이 이 시간 안에 끝나지 않으면 포기한다. 9픽셀 전송이 약 0.87ms 다. */
#define WS2812_DMA_TIMEOUT_MS 50U
/**
 * DMA 스트림의 EN 비트가 내려가기를 기다리는 최대 반복 수.
 *
 * 하드웨어는 진행 중인 버스트를 끝낸 뒤 EN 을 내리므로 보통 수 사이클이면 끝난다.
 * 그래도 무한 루프로 두지 않는다 -- 여기서 멈추면 태스크가 통째로 죽어 ACK 와 진단
 * 로그까지 함께 사라지고, 정작 원인은 아무 데도 남지 않는다.
 */
#define WS2812_DMA_DISABLE_SPIN_LIMIT 100000U

/**
 * 이 드라이버가 전제하는 타이머 입력 클럭.
 *
 * ARR/T0H/T1H 를 틱 수로 못박았으므로 클럭이 바뀌면 파형이 통째로 어긋난다. 예전 구현은
 * 클럭을 읽어 나노초에서 틱을 계산했지만 지금은 상수 고정이라, 대신 neopixel_init() 이
 * 실제 클럭을 확인하고 다르면 HAL_ERROR 를 돌려준다.
 */
#define WS2812_TIMER_CLOCK_HZ 84000000U

/** 9픽셀 x 24비트 = 216 슬롯 */
#define WS2812_DATA_SLOT_COUNT (NEOPIXEL_PIXEL_COUNT * WS2812_BITS_PER_PIXEL)
/** 240 + 216 + 240 = 696 슬롯 */
#define WS2812_DMA_BUFFER_SIZE ((WS2812_RESET_SLOT_COUNT * 2U) + WS2812_DATA_SLOT_COUNT)
/** 픽셀 데이터가 시작되는 슬롯 번호 */
#define WS2812_DATA_SLOT_FIRST WS2812_RESET_SLOT_COUNT

/* 위 상수들이 서로 어긋나면 파형이 조용히 망가진다. 빌드 시점에 막는다.
 * 특히 T0H/T1H 를 -D 로 덮어썼을 때 여기서 걸린다.
 * 메시지를 ASCII 로 적는 이유: 컴파일러가 오류 문자열을 그대로 토해내는데 한글이면
 * 콘솔 인코딩에 따라 깨져서 정작 무엇이 틀렸는지 읽을 수 없다. */

/* 논리 0 의 High 폭이 논리 1 보다 짧아야 한다 */
_Static_assert(WS2812_T0H_TICKS < WS2812_T1H_TICKS,
               "WS2812_T0H_TICKS must be shorter than WS2812_T1H_TICKS");
/* High 폭이 비트 주기를 넘으면 그 비트가 계속 High 로 남아 회선이 멎는다 */
_Static_assert(WS2812_T1H_TICKS < WS2812_TIMER_ARR,
               "WS2812_T1H_TICKS must be shorter than the bit period (ARR)");

/* ---------------------------------------------------------------------------
 * 데이터시트 창(window) 검사
 *
 * 위 [비트 폭] 표의 교집합을 그대로 옮긴 것이다. High 뿐 아니라 Low 까지 검사하는 것이
 * 핵심이다 -- 예전 값(T1H 67)은 High 상한과 Low 하한을 동시에 벗어났는데, High 만 보던
 * 시절에는 그게 드러나지 않았다. 이 검사가 있으면 같은 실수가 빌드에서 멎는다.
 *
 * 하한은 올림, 상한은 내림으로 tick 을 구한다. 경계에서 규격 밖을 통과시키지 않는 쪽이다.
 * ------------------------------------------------------------------------ */
#define WS2812_TICKS_AT_LEAST(ns) \
  ((((ns) * (WS2812_TIMER_CLOCK_HZ / 1000000U)) + 999U) / 1000U)
#define WS2812_TICKS_AT_MOST(ns) \
  (((ns) * (WS2812_TIMER_CLOCK_HZ / 1000000U)) / 1000U)
/** 한 비트의 Low 구간 = 주기 - High 구간. 타이머가 주기를 고정하므로 자동으로 따라온다. */
#define WS2812_T0L_TICKS ((WS2812_TIMER_ARR + 1U) - WS2812_T0H_TICKS)
#define WS2812_T1L_TICKS ((WS2812_TIMER_ARR + 1U) - WS2812_T1H_TICKS)

_Static_assert(WS2812_T0H_TICKS >= WS2812_TICKS_AT_LEAST(250U),
               "T0H is shorter than the WS2811/WS2812/WS2812B common minimum (250ns)");
_Static_assert(WS2812_T0H_TICKS <= WS2812_TICKS_AT_MOST(400U),
               "T0H is longer than the WS2811/WS2812/WS2812B common maximum (400ns)");
_Static_assert(WS2812_T1H_TICKS >= WS2812_TICKS_AT_LEAST(650U),
               "T1H is shorter than the WS2811/WS2812/WS2812B common minimum (650ns)");
_Static_assert(WS2812_T1H_TICKS <= WS2812_TICKS_AT_MOST(750U),
               "T1H is longer than the WS2811/WS2812/WS2812B common maximum (750ns)");
_Static_assert(WS2812_T0L_TICKS >= WS2812_TICKS_AT_LEAST(850U),
               "T0L is shorter than the WS2811/WS2812/WS2812B common minimum (850ns)");
_Static_assert(WS2812_T0L_TICKS <= WS2812_TICKS_AT_MOST(950U),
               "T0L is longer than the WS2811/WS2812/WS2812B common maximum (950ns)");
/* 이 줄이 예전 값(T1H 67 -> T1L 452.4ns)을 잡아낸다. 관측된 색 어긋남의 유력한 원인이다. */
_Static_assert(WS2812_T1L_TICKS >= WS2812_TICKS_AT_LEAST(500U),
               "T1L is shorter than the WS2811 minimum (500ns): bits will slip");
_Static_assert(WS2812_T1L_TICKS <= WS2812_TICKS_AT_MOST(600U),
               "T1L is longer than the WS2812B maximum (600ns)");
/* 비트 주기가 800kHz(1.25us)가 아니다 */
_Static_assert((WS2812_TIMER_CLOCK_HZ / (WS2812_TIMER_ARR + 1U)) == 800000U,
               "bit period must be 800kHz: check WS2812_TIMER_ARR / clock");
/* DMA 의 NDTR 은 16비트다. 픽셀 수나 리셋 길이를 키울 때 여기서 걸린다. */
_Static_assert(WS2812_DMA_BUFFER_SIZE <= 65535U,
               "DMA transfer length exceeds the 16-bit NDTR limit");

/* ---------------------------------------------------------------------------
 * 채널별 하드웨어 서술표
 *
 * 채널마다 타이머도 DMA 스트림도 레지스터 위치도 다르다. CH1 은 CCMR1/CCR1 을 쓰고
 * CH3 은 CCMR2/CCR3 을 쓰며, Stream1 은 LISR/LIFCR, Stream4 는 HISR/HIFCR 을 쓴다.
 * 그 차이를 전부 이 표 하나에 모아 두고 나머지 코드는 표만 본다.
 * ------------------------------------------------------------------------ */
typedef struct {
    TIM_TypeDef *timer;
    volatile uint32_t *ccmr;       /**< CH1이면 CCMR1, CH3이면 CCMR2 */
    uint32_t ccmr_pwm_bits;        /**< 해당 채널 자리의 PWM1 + 프리로드 비트 */
    uint32_t ccer_enable;
    uint32_t dier_dma;             /**< 그 채널의 CC DMA 요청 허용 비트 */
    volatile uint32_t *ccr;
    DMA_Stream_TypeDef *stream;
    volatile uint32_t *dma_isr;    /**< Stream 0~3은 LISR, 4~7은 HISR */
    volatile uint32_t *dma_ifcr;
    uint32_t dma_chsel;
    uint32_t dma_tc_flag;
    uint32_t dma_error_flags;
    uint32_t dma_clear_flags;
    GPIO_TypeDef *port;
    uint16_t pin;
    uint8_t alternate;
} neopixel_hw_t;

static const neopixel_hw_t neopixel_hw[NEOPIXEL_CH_COUNT] = {
    /* A : PA6(D12) / TIM3 CH1 / AF2 / DMA1 Stream4 Channel 5 */
    [NEOPIXEL_CH_A] = {
        .timer = TIM3,
        .ccmr = &TIM3->CCMR1,
        .ccmr_pwm_bits = TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1PE,
        .ccer_enable = TIM_CCER_CC1E,
        .dier_dma = TIM_DIER_CC1DE,
        .ccr = &TIM3->CCR1,
        .stream = DMA1_Stream4,
        .dma_isr = &DMA1->HISR,
        .dma_ifcr = &DMA1->HIFCR,
        .dma_chsel = DMA_SxCR_CHSEL_0 | DMA_SxCR_CHSEL_2,   /* 5 = 0b101 */
        .dma_tc_flag = DMA_HISR_TCIF4,
        .dma_error_flags = DMA_HISR_TEIF4 | DMA_HISR_DMEIF4 | DMA_HISR_FEIF4,
        .dma_clear_flags = DMA_HIFCR_CFEIF4 | DMA_HIFCR_CDMEIF4 | DMA_HIFCR_CTEIF4 |
                           DMA_HIFCR_CHTIF4 | DMA_HIFCR_CTCIF4,
        .port = NEOPIXEL_A_GPIO_Port,
        .pin = NEOPIXEL_A_Pin,
        .alternate = GPIO_AF2_TIM3,
    },
    /* B : PA7(D11) / TIM3 CH2 / AF2 / DMA1 Stream5 Channel 5
     * A 와 같은 TIM3 라 CCMR1 과 CCER 을 공유한다. 그래서 neopixel_init() 이 이 두
     * 레지스터에만 |= 로 자기 비트를 얹는다 -- 대입하면 서로를 지운다. */
    [NEOPIXEL_CH_B] = {
        .timer = TIM3,
        .ccmr = &TIM3->CCMR1,   /* CH2 도 CCMR1 에 있다 (CCMR2 는 CH3/CH4) */
        .ccmr_pwm_bits = TIM_CCMR1_OC2M_1 | TIM_CCMR1_OC2M_2 | TIM_CCMR1_OC2PE,
        .ccer_enable = TIM_CCER_CC2E,
        .dier_dma = TIM_DIER_CC2DE,
        .ccr = &TIM3->CCR2,
        .stream = DMA1_Stream5,
        .dma_isr = &DMA1->HISR,
        .dma_ifcr = &DMA1->HIFCR,
        .dma_chsel = DMA_SxCR_CHSEL_0 | DMA_SxCR_CHSEL_2,   /* 5 = 0b101 */
        .dma_tc_flag = DMA_HISR_TCIF5,
        .dma_error_flags = DMA_HISR_TEIF5 | DMA_HISR_DMEIF5 | DMA_HISR_FEIF5,
        .dma_clear_flags = DMA_HIFCR_CFEIF5 | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CTEIF5 |
                           DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5,
        .port = NEOPIXEL_B_GPIO_Port,
        .pin = NEOPIXEL_B_Pin,
        .alternate = GPIO_AF2_TIM3,
    },
};

/**
 * PWM 듀티 버퍼. 두 채널이 함께 쓴다.
 *
 * 송신 함수가 전송이 끝날 때까지 기다리는 블로킹 함수이고 태스크 문맥에서만 불리므로
 * 두 채널의 전송이 겹치지 않는다. 채널마다 따로 두면 1.4KB 를 더 먹는다.
 *
 * 앞뒤 리셋 구간은 항상 0 이므로 초기화 때 한 번 지우고 그 뒤로는 데이터 구간
 * ([240, 456))만 갈아 쓴다. 두 채널이 이 구간을 번갈아 덮어쓰는 것은 무해하다 --
 * 송신 직전에 자기 색으로 다시 채우기 때문이다.
 */
static uint16_t pwm_data[WS2812_DMA_BUFFER_SIZE];

/** 이 채널이 초기화되었는지. 초기화 전에 송신하면 타이머가 죽어 있어 아무것도 나가지 않는다. */
static uint8_t channel_ready[NEOPIXEL_CH_COUNT];

/**
 * @brief 한 바이트를 PWM 듀티 8개로 펼친다.
 * @param value 펼칠 바이트
 * @param slot_index 다음 듀티를 쓸 버퍼 위치. 쓴 만큼 증가한다.
 * @details WS2812 규격대로 MSB(0x80) 부터 LSB(0x01) 순으로 나간다.
 */
static void encode_byte(uint8_t value, uint32_t *slot_index)
{
    for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1U) {
        pwm_data[*slot_index] = ((value & mask) != 0U) ? WS2812_T1H_TICKS : WS2812_T0H_TICKS;
        ++(*slot_index);
    }
}

/**
 * @brief DMA 스트림을 내리고 EN 이 실제로 0 이 될 때까지 기다린다.
 * @param stream 대상 스트림
 * @return 1 내려갔다, 0 제한 반복 안에 내려가지 않았다
 * @details 하드웨어는 진행 중인 버스트를 끝낸 뒤 EN 을 내리므로 보통 즉시 끝난다.
 * 무한 루프로 두지 않는 이유는 여기서 멈추면 태스크가 통째로 죽어 ACK 와 진단 로그까지
 * 사라지기 때문이다 -- 원인을 남기려면 빠져나와 오류로 돌려주는 편이 낫다.
 */
static uint8_t dma_stream_disable(DMA_Stream_TypeDef *stream)
{
    uint32_t guard = WS2812_DMA_DISABLE_SPIN_LIMIT;

    stream->CR &= ~DMA_SxCR_EN;
    while (((stream->CR & DMA_SxCR_EN) != 0U) && (guard != 0U)) {
        --guard;
    }

    return (guard != 0U) ? 1U : 0U;
}

/**
 * @brief 준비된 버퍼를 지정한 채널의 타이머 + DMA 로 한 프레임 내보낸다.
 * @param hw 채널 하드웨어 서술
 * @param slot_count 내보낼 슬롯 수 (PRE LOW + 데이터 + POST LOW)
 * @return HAL_OK 전송 완료
 * @return HAL_ERROR DMA 오류(TE/DME/FE)이거나 스트림을 내리지 못했다
 * @return HAL_TIMEOUT WS2812_DMA_TIMEOUT_MS 안에 끝나지 않았다
 * @details 시작 순서를 지키는 것이 이 함수의 전부다.
 *
 *   타이머 정지 -> CNT = 0 -> CCR = 0 -> Update Event -> DMA 설정 -> DMA Enable
 *   -> CC DMA 요청 허용 -> 타이머 Counter Enable
 *
 * 첫 데이터 비트를 CCR 에 미리 실어 두고 DMA 를 두 번째 슬롯부터 시작하는 방식은 쓰지 않는다.
 * 그렇게 하면 폭이 어긋날 수 있는 첫 펄스에 실제 데이터가 실린다. 여기서는 CCR = 0 에서
 * 출발하고, 앞의 240 슬롯이 전부 LOW 라 첫 펄스 자체가 없다.
 *
 * DIER 를 Update Event 뒤에 켜는 이유: EGR = UG 로 CNT 가 0 이 되는 순간 CCR 도 0 이라
 * 매치가 잡힐 수 있는데, 그때 요청이 나가면 슬롯 하나를 먼저 소비해 전체가 한 칸씩 밀린다.
 * 카운터를 돌리기 전이라면 DMA Enable 앞이든 뒤든 상관없다 -- 요청 자체가 없다.
 *
 * 전송이 끝나면 타이머를 세우고 CCR = 0 + UG 로 출력을 LOW 로 확정한다. 켜 둔 채로 두면
 * 마지막 듀티가 계속 반복되어 스트립이 다음 프레임의 시작을 알아보지 못한다.
 */
static HAL_StatusTypeDef ws2812_transmit(const neopixel_hw_t *hw, uint32_t slot_count)
{
    uint32_t start_tick;
    HAL_StatusTypeDef status = HAL_OK;

    /* 앞 전송이 남아 있으면 먼저 내린다. */
    if (dma_stream_disable(hw->stream) == 0U) {
        return HAL_ERROR;
    }

    /* 1) 타이머 정지  2) CNT = 0  3) CCR = 0  4) Update Event */
    hw->timer->CR1 &= ~TIM_CR1_CEN;
    hw->timer->DIER = 0U;
    hw->timer->CNT = 0U;
    *hw->ccr = 0U;
    hw->timer->EGR = TIM_EGR_UG;   /* 프리로드된 ARR/CCR(=0)을 지금 반영시킨다 */
    hw->timer->SR = 0U;

    /* 5) DMA 설정 */
    *hw->dma_ifcr = hw->dma_clear_flags;
    hw->stream->PAR = (uint32_t)hw->ccr;
    hw->stream->M0AR = (uint32_t)&pwm_data[0];   /* 첫 슬롯부터 그대로 넘긴다 */
    hw->stream->NDTR = slot_count;
    hw->stream->FCR = 0U;                        /* 직접 모드 */
    hw->stream->CR = hw->dma_chsel |
                     DMA_SxCR_DIR_0 |            /* 메모리 -> 주변장치 */
                     DMA_SxCR_MINC |
                     DMA_SxCR_PSIZE_0 |          /* 16비트 */
                     DMA_SxCR_MSIZE_0 |          /* 16비트 */
                     DMA_SxCR_PL_1;              /* 우선순위 High */

    /* 6) DMA Enable */
    hw->stream->CR |= DMA_SxCR_EN;

    /* 7) 마지막으로 타이머 카운터 Enable */
    hw->timer->DIER = hw->dier_dma;
    hw->timer->CR1 |= TIM_CR1_CEN;

    start_tick = HAL_GetTick();
    while ((*hw->dma_isr & hw->dma_tc_flag) == 0U) {
        if ((*hw->dma_isr & hw->dma_error_flags) != 0U) {
            status = HAL_ERROR;
            break;
        }
        if ((uint32_t)(HAL_GetTick() - start_tick) >= WS2812_DMA_TIMEOUT_MS) {
            status = HAL_TIMEOUT;
            break;
        }
    }

    /* 전송 완료 처리: 타이머 STOP -> CCR = 0 -> Update Event. 데이터 핀은 LOW 로 남는다.
     * DIER 를 가장 먼저 끄는 이유: 아래 UG 로 CNT 가 0 이 되는데 CCR 도 0 이라, 요청이
     * 살아 있으면 끝난 전송에 대해 매치가 하나 더 잡힐 수 있다. */
    hw->timer->DIER = 0U;
    hw->timer->CR1 &= ~TIM_CR1_CEN;
    *hw->ccr = 0U;
    hw->timer->EGR = TIM_EGR_UG;

    if (dma_stream_disable(hw->stream) == 0U) {
        status = HAL_ERROR;
    }
    *hw->dma_ifcr = hw->dma_clear_flags;

    return status;
}

/**
 * @brief TIM3 에 실제로 들어가는 클럭을 계산한다.
 * @return 타이머 입력 클럭 (Hz)
 * @details TIM3 은 APB1 에 달려 있다. APB1 분주가 1 이 아니면 타이머 클럭은
 * PCLK1 의 2배가 된다(이 보드는 PCLK1 42MHz -> 타이머 84MHz). RM0368 의 클럭 트리 규칙이다.
 */
static uint32_t timer_input_clock_hz(void)
{
    uint32_t hz = HAL_RCC_GetPCLK1Freq();

    if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV1) {
        hz *= 2U;
    }

    return hz;
}

HAL_StatusTypeDef neopixel_init(neopixel_channel_t channel)
{
    GPIO_InitTypeDef gpio_init = {0};
    const neopixel_hw_t *hw;

    if ((uint32_t)channel >= (uint32_t)NEOPIXEL_CH_COUNT) {
        return HAL_ERROR;
    }

    /* ARR/T0H/T1H 가 84MHz 기준 틱 수로 고정되어 있다. 클럭이 다르면 비트 폭이 통째로
     * 어긋나 스트립이 아무 색이나 내거나 아예 반응하지 않는데, 파형을 재 보기 전에는
     * 원인을 알 수 없다. 여기서 먼저 걸러 호출자에게 알린다. */
    if (timer_input_clock_hz() != WS2812_TIMER_CLOCK_HZ) {
        return HAL_ERROR;
    }

    hw = &neopixel_hw[channel];

    /* 두 채널 모두 PA6/PA7 + TIM3 라 켤 클럭이 같다 -- 채널로 갈릴 것이 없다. */
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();

    /* 이 핀은 GPIO 출력이 아니라 타이머 출력(AF)으로 쓴다. */
    gpio_init.Pin = hw->pin;
    gpio_init.Mode = GPIO_MODE_AF_PP;
    gpio_init.Pull = GPIO_NOPULL;
    gpio_init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio_init.Alternate = hw->alternate;
    HAL_GPIO_Init(hw->port, &gpio_init);

    /* PWM 모드 1 + CCR 프리로드. TIM3 은 범용 타이머라 BDTR(MOE)이 없다 --
     * 고급 타이머(TIM1)용 코드를 옮겨올 때 그 줄을 빼야 한다.
     *
     * CCMR1 과 CCER 만 |= 로 얹는다. 두 채널이 TIM3 하나를 나눠 쓰므로 이 두 레지스터에는
     * 남의 채널 비트도 들어 있다 -- 통째로 대입하면 나중에 초기화되는 B 가 A 의 OC1M/CC1E
     * 를 지워 PA6 이 영영 켜지지 않는다. 같은 채널을 두 번 불러도 같은 비트를 다시 얹는
     * 것이라 멱등은 유지된다.
     *
     * 나머지(CR1/CR2/SMCR/PSC/ARR)는 채널과 무관한 타이머 공용 설정이고 두 채널이 같은
     * 값을 넣으므로 대입 그대로 둔다. */
    hw->timer->CR1 = TIM_CR1_ARPE;
    hw->timer->CR2 = 0U;
    hw->timer->SMCR = 0U;
    hw->timer->DIER = 0U;
    *hw->ccmr |= hw->ccmr_pwm_bits;
    hw->timer->CCER |= hw->ccer_enable;
    hw->timer->PSC = 0U;
    hw->timer->ARR = WS2812_TIMER_ARR;
    hw->timer->CNT = 0U;
    *hw->ccr = 0U;
    hw->timer->EGR = TIM_EGR_UG;
    hw->timer->SR = 0U;
    /* CEN 은 건드리지 않는다 -- 타이머는 멈춘 채로 두고 송신할 때만 돈다. */

    hw->stream->CR = 0U;
    if (dma_stream_disable(hw->stream) == 0U) {
        return HAL_ERROR;
    }
    hw->stream->FCR = 0U;
    *hw->dma_ifcr = hw->dma_clear_flags;

    /* 앞뒤 리셋 구간을 0 으로 만들어 둔다. 이후 데이터 구간만 갈아 쓰므로 다시 지울 일이 없다.
     * 두 채널이 버퍼를 공유하므로 두 번째 init 이 다시 지워도 무해하다. */
    (void)memset(pwm_data, 0, sizeof(pwm_data));

    channel_ready[channel] = 1U;

    /* 라인을 Low 로 충분히 눌러 스트립이 리셋을 인식하게 한다. 이게 없으면 첫 프레임의
     * 앞부분이 직전 잡음에 이어 붙어 엉뚱한 픽셀에 실린다. */
    HAL_Delay(1U);

    return HAL_OK;
}

HAL_StatusTypeDef neopixel_show_solid(neopixel_channel_t channel,
                                      uint16_t pixel_count,
                                      uint8_t red,
                                      uint8_t green,
                                      uint8_t blue)
{
    const neopixel_hw_t *hw;
    uint32_t slot_index = WS2812_DATA_SLOT_FIRST;
    uint32_t data_end;
    uint16_t index;
    HAL_StatusTypeDef status;

    if ((uint32_t)channel >= (uint32_t)NEOPIXEL_CH_COUNT) {
        return HAL_ERROR;
    }
    if (pixel_count == 0U || pixel_count > NEOPIXEL_MAX_PIXEL_COUNT) {
        return HAL_ERROR;
    }
    if (channel_ready[channel] == 0U) {
        return HAL_ERROR;   /* neopixel_init() 을 부르지 않았다 */
    }
    hw = &neopixel_hw[channel];

    /* 회선에 나가는 순서는 이 스트립 기준 BRG 다. 인자는 사람이 읽기 쉬운 RGB 라 여기서
     * 재배열한다.
     *
     * WS2812B 규격은 GRB 지만 이 보드에 물린 12V 스트립은 WS2811 이 LED 3개를 묶어 구동하는
     * 물건이고, WS2811 모듈은 제조사가 RGB 배선을 자유롭게 뽑는다. 규격이 아니라 실물이
     * 근거다. GRB 로 내보냈을 때 세 등급이 이렇게 나왔다:
     *
     *   의도    GRB 로 나간 바이트   실제로 보인 색      도출
     *   ------  -------------------  ------------------  --------------
     *   초록    FF 00 00             파랑                1번째 = B
     *   노랑    FF FF 00             분홍(자홍=R+B)      2번째 = R
     *   빨강    00 FF 00             빨강                2번째 = R (확인)
     *
     * 세 줄을 동시에 만족하는 순서는 B,R,G 하나뿐이고 남은 3번째가 G 다. 노랑이 자홍으로
     * 나온 것이 결정적이다 -- 그 한 줄이 없으면 BRG 와 BGR 을 가를 수 없다.
     *
     * 이 순서를 적용하면 세 등급이 모두 맞는다:
     *   초록 -> 00 00 FF (G=255)        노랑 -> 00 FF FF (R=G=255)
     *   빨강 -> 00 FF 00 (R=255)
     *
     * 참고: Desktop/neopixeltest 기준 구현은 GRB 라 이 스트립에서 초록과 파랑이 서로
     * 바뀐다. 빨강만 우연히 맞아떨어져 정상으로 보일 뿐이니 그쪽을 근거로 삼지 말 것.
     *
     * 스트립을 다른 물건으로 갈면 여기부터 의심할 것. 색이 자리만 바뀐 듯 어긋나면 이
     * 순서 문제이고, 색이 흔들리거나 아예 안 켜지면 그때가 타이밍(T0H/T1H) 문제다. */
    for (index = 0U; index < pixel_count; ++index) {
        encode_byte(blue, &slot_index);
        encode_byte(red, &slot_index);
        encode_byte(green, &slot_index);
    }
    data_end = slot_index;

    /* pixel_count 가 최대보다 작으면 남은 데이터 슬롯을 0(LOW)으로 되돌린다. 앞 프레임의
     * 듀티가 남아 있으면 이번 전송의 리셋 구간에 실려 프레임 끝을 흐린다. */
    while (slot_index < (WS2812_DATA_SLOT_FIRST + WS2812_DATA_SLOT_COUNT)) {
        pwm_data[slot_index] = 0U;
        ++slot_index;
    }

    /* PRE LOW + 데이터 + POST LOW. pixel_count 가 9 면 240 + 216 + 240 = 696 이다. */
    status = ws2812_transmit(hw, data_end + WS2812_RESET_SLOT_COUNT);

    if (status == HAL_OK) {
        /* 리셋 구간을 한 번 더 확실히 준다. 연속 호출 사이에 프레임이 붙는 것을 막는다. */
        HAL_Delay(1U);
    }

    return status;
}
