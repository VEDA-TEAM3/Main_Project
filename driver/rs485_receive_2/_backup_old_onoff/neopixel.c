#include "neopixel.h"

#include "main.h"

#include <string.h>

/*
 * NeoPixel(WS2812 호환) 2채널 드라이버 구현.
 * 배선/핀 배정/12V 스트립 주의사항은 neopixel.h 의 머리말에 정리되어 있다.
 *
 * 이 파일은 TIM3, TIM2, DMA1 Stream4, DMA1 Stream1 을 단독으로 소유한다.
 * CubeMX 가 만든 TIM/DMA 핸들은 쓰지 않는다 -- 비트 단위로 CCR 을 갈아끼워야 해서
 * HAL 의 PWM API 로는 표현할 수 없기 때문이다.
 */

enum {
    /** 비트 하나의 길이. 800kHz = 1.25us */
    WS2812_FREQUENCY_HZ = 800000U,
    /** 한 픽셀은 GRB 3바이트 = 24비트 */
    WS2812_BITS_PER_PIXEL = 24U,
    /** 데이터 뒤에 붙이는 Low 구간. 240 x 1.25us = 300us 로 규격의 리셋 시간을 넘긴다. */
    WS2812_RESET_SLOT_COUNT = 240U,
    /** 전송이 이 시간 안에 끝나지 않으면 포기한다. 300픽셀 송신이 약 9ms 다. */
    WS2812_DMA_TIMEOUT_MSEC = 50U
};

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
    uint32_t dier_dma;
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
    /* B : PB10(D6) / TIM2 CH3 / AF1 / DMA1 Stream1 Channel 3 */
    [NEOPIXEL_CH_B] = {
        .timer = TIM2,
        .ccmr = &TIM2->CCMR2,
        .ccmr_pwm_bits = TIM_CCMR2_OC3M_1 | TIM_CCMR2_OC3M_2 | TIM_CCMR2_OC3PE,
        .ccer_enable = TIM_CCER_CC3E,
        .dier_dma = TIM_DIER_CC3DE,
        .ccr = &TIM2->CCR3,
        .stream = DMA1_Stream1,
        .dma_isr = &DMA1->LISR,
        .dma_ifcr = &DMA1->LIFCR,
        .dma_chsel = DMA_SxCR_CHSEL_0 | DMA_SxCR_CHSEL_1,   /* 3 = 0b011 */
        .dma_tc_flag = DMA_LISR_TCIF1,
        .dma_error_flags = DMA_LISR_TEIF1 | DMA_LISR_DMEIF1 | DMA_LISR_FEIF1,
        .dma_clear_flags = DMA_LIFCR_CFEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CTEIF1 |
                           DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1,
        .port = NEOPIXEL_B_GPIO_Port,
        .pin = NEOPIXEL_B_Pin,
        .alternate = GPIO_AF1_TIM2,
    },
};

/**
 * 채널별 색 버퍼. 픽셀당 GRB 3바이트를 회선에 나가는 순서 그대로 담는다.
 * 300 x 3 x 2채널 = 1800바이트.
 */
static uint8_t pixel_grb[NEOPIXEL_CH_COUNT][NEOPIXEL_MAX_PIXEL_COUNT][3];

/**
 * PWM 듀티 버퍼. 두 채널이 함께 쓴다.
 *
 * 송신 함수가 전송이 끝날 때까지 기다리는 블로킹 함수이고 태스크 문맥에서만 불리므로
 * 두 채널의 전송이 겹치지 않는다. 채널마다 따로 두면 15KB 를 더 먹는다.
 */
static uint16_t pwm_data[(NEOPIXEL_MAX_PIXEL_COUNT * WS2812_BITS_PER_PIXEL) + WS2812_RESET_SLOT_COUNT];

static uint16_t pwm_period_ticks;
static uint16_t zero_high_ticks;
static uint16_t one_high_ticks;

/** 이 채널이 초기화되었는지. 초기화 전에 송신하면 타이머가 죽어 있어 아무것도 나가지 않는다. */
static uint8_t channel_ready[NEOPIXEL_CH_COUNT];

/**
 * @brief 비트 길이와 0/1 High 폭을 지금 클럭에 맞춰 계산한다.
 * @details TIM2 와 TIM3 는 둘 다 APB1 에 달려 있다. APB1 분주가 1이 아니면 타이머 클럭은
 * PCLK1 의 2배가 된다(이 보드는 PCLK1 42MHz -> 타이머 84MHz).
 * 84MHz / 800kHz = 105 틱이 한 비트다.
 *
 * High 폭은 neopixel.h 의 NEOPIXEL_T0H_NS / NEOPIXEL_T1H_NS 에서 나온다. 스트립 종류에
 * 따라 요구 시간이 달라 조정이 필요할 수 있어 나노초로 적어 두고 여기서 틱으로 바꾼다.
 */
static void compute_bit_timing(void)
{
    uint32_t timer_clock_hz = HAL_RCC_GetPCLK1Freq();
    uint32_t ticks_per_us;

    if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV1) {
        timer_clock_hz *= 2U;
    }

    pwm_period_ticks = (uint16_t)(timer_clock_hz / WS2812_FREQUENCY_HZ);

    /* 1us 당 틱 수. 84MHz 면 84. 나노초 -> 틱 변환에 쓴다. */
    ticks_per_us = timer_clock_hz / 1000000U;

    zero_high_ticks = (uint16_t)((NEOPIXEL_T0H_NS * ticks_per_us) / 1000U);
    one_high_ticks = (uint16_t)((NEOPIXEL_T1H_NS * ticks_per_us) / 1000U);

    /* 듀티가 주기를 넘으면 그 비트는 영원히 High 로 남아 회선이 멎는다. 잘라 둔다. */
    if (zero_high_ticks >= pwm_period_ticks) {
        zero_high_ticks = (uint16_t)(pwm_period_ticks - 1U);
    }
    if (one_high_ticks >= pwm_period_ticks) {
        one_high_ticks = (uint16_t)(pwm_period_ticks - 1U);
    }
}

/**
 * @brief 한 바이트를 PWM 듀티 8개로 펼친다. MSB 부터 나간다(WS2812 규격).
 */
static void encode_byte(uint8_t value, uint32_t *data_index)
{
    for (uint8_t mask = 0x80U; mask != 0U; mask >>= 1U) {
        pwm_data[*data_index] = ((value & mask) != 0U) ? one_high_ticks : zero_high_ticks;
        ++(*data_index);
    }
}

HAL_StatusTypeDef neopixel_init(neopixel_channel_t channel)
{
    GPIO_InitTypeDef gpio_init = {0};
    const neopixel_hw_t *hw;

    if ((uint32_t)channel >= (uint32_t)NEOPIXEL_CH_COUNT) {
        return HAL_ERROR;
    }
    hw = &neopixel_hw[channel];

    compute_bit_timing();

    __HAL_RCC_DMA1_CLK_ENABLE();
    if (channel == NEOPIXEL_CH_A) {
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_TIM3_CLK_ENABLE();
    } else {
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_TIM2_CLK_ENABLE();
    }

    gpio_init.Pin = hw->pin;
    gpio_init.Mode = GPIO_MODE_AF_PP;
    gpio_init.Pull = GPIO_NOPULL;
    gpio_init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio_init.Alternate = hw->alternate;
    HAL_GPIO_Init(hw->port, &gpio_init);

    /* PWM 모드 1 + CCR 프리로드. TIM2/TIM3 모두 범용 타이머라 BDTR(MOE)이 없다 --
     * 고급 타이머(TIM1)용 코드를 옮겨올 때 그 줄을 빼야 한다. */
    hw->timer->CR1 = TIM_CR1_ARPE;
    hw->timer->CR2 = 0U;
    hw->timer->SMCR = 0U;
    hw->timer->DIER = 0U;
    *hw->ccmr = hw->ccmr_pwm_bits;
    hw->timer->CCER = hw->ccer_enable;
    hw->timer->PSC = 0U;
    hw->timer->ARR = (uint32_t)pwm_period_ticks - 1U;
    *hw->ccr = 0U;
    hw->timer->EGR = TIM_EGR_UG;

    hw->stream->CR = 0U;
    hw->stream->FCR = 0U;
    *hw->dma_ifcr = hw->dma_clear_flags;

    (void)memset(pixel_grb[channel], 0, sizeof(pixel_grb[channel]));
    channel_ready[channel] = 1U;

    /* 라인을 Low 로 충분히 눌러 스트립이 리셋을 인식하게 한다. 이게 없으면 첫 프레임의
     * 앞부분이 직전 잡음에 이어 붙어 엉뚱한 픽셀에 실린다. */
    HAL_Delay(1U);

    return HAL_OK;
}

HAL_StatusTypeDef neopixel_set_pixel(neopixel_channel_t channel, uint16_t index,
                                     uint8_t red, uint8_t green, uint8_t blue)
{
    if ((uint32_t)channel >= (uint32_t)NEOPIXEL_CH_COUNT) {
        return HAL_ERROR;
    }
    if (index >= NEOPIXEL_MAX_PIXEL_COUNT) {
        return HAL_ERROR;
    }

    /* 회선에 나가는 순서가 GRB 라 버퍼에도 그 순서로 담는다. 인자는 사람이 읽기 쉬운 RGB 다. */
    pixel_grb[channel][index][0] = green;
    pixel_grb[channel][index][1] = red;
    pixel_grb[channel][index][2] = blue;

    return HAL_OK;
}

HAL_StatusTypeDef neopixel_fill(neopixel_channel_t channel, uint16_t count,
                                uint8_t red, uint8_t green, uint8_t blue)
{
    uint16_t index;

    if ((uint32_t)channel >= (uint32_t)NEOPIXEL_CH_COUNT) {
        return HAL_ERROR;
    }
    if (count == 0U || count > NEOPIXEL_MAX_PIXEL_COUNT) {
        return HAL_ERROR;
    }

    for (index = 0U; index < count; ++index) {
        pixel_grb[channel][index][0] = green;
        pixel_grb[channel][index][1] = red;
        pixel_grb[channel][index][2] = blue;
    }

    return HAL_OK;
}

HAL_StatusTypeDef neopixel_show(neopixel_channel_t channel, uint16_t count)
{
    const neopixel_hw_t *hw;
    uint32_t data_index = 0U;
    uint32_t total_slots;
    uint16_t index;
    uint32_t transfer_start_msec;
    HAL_StatusTypeDef transfer_status = HAL_OK;

    if ((uint32_t)channel >= (uint32_t)NEOPIXEL_CH_COUNT) {
        return HAL_ERROR;
    }
    if (count == 0U || count > NEOPIXEL_MAX_PIXEL_COUNT) {
        return HAL_ERROR;
    }
    if (channel_ready[channel] == 0U) {
        return HAL_ERROR;   /* neopixel_init() 을 부르지 않았다 */
    }
    hw = &neopixel_hw[channel];

    for (index = 0U; index < count; ++index) {
        encode_byte(pixel_grb[channel][index][0], &data_index);
        encode_byte(pixel_grb[channel][index][1], &data_index);
        encode_byte(pixel_grb[channel][index][2], &data_index);
    }

    /* 뒤에 0 듀티를 채워 리셋(Low) 구간을 만든다. 이게 있어야 스트립이 프레임 끝을 알고
     * 받은 색을 실제로 반영한다. */
    total_slots = ((uint32_t)count * WS2812_BITS_PER_PIXEL) + WS2812_RESET_SLOT_COUNT;
    while (data_index < total_slots) {
        pwm_data[data_index] = 0U;
        ++data_index;
    }

    hw->stream->CR &= ~DMA_SxCR_EN;
    while ((hw->stream->CR & DMA_SxCR_EN) != 0U) {
    }

    *hw->dma_ifcr = hw->dma_clear_flags;
    hw->stream->PAR = (uint32_t)hw->ccr;
    /* 첫 듀티는 아래에서 CCR 에 직접 실어 두므로 DMA 는 두 번째 원소부터 넘긴다.
     * 이렇게 해야 타이머가 도는 첫 비트가 어긋나지 않는다. */
    hw->stream->M0AR = (uint32_t)&pwm_data[1];
    hw->stream->NDTR = data_index - 1U;
    hw->stream->FCR = 0U;
    hw->stream->CR = hw->dma_chsel | DMA_SxCR_DIR_0 | DMA_SxCR_MINC |
                     DMA_SxCR_PSIZE_0 | DMA_SxCR_MSIZE_0 | DMA_SxCR_PL_1;

    hw->timer->CR1 &= ~TIM_CR1_CEN;
    hw->timer->CNT = 0U;
    *hw->ccr = pwm_data[0];
    /* UG 를 DIER 보다 먼저 넣는다. 순서가 반대면 이 업데이트 이벤트가 DMA 요청을 하나
     * 더 만들어 첫 비트가 밀린다. */
    hw->timer->EGR = TIM_EGR_UG;
    hw->timer->SR = 0U;
    hw->timer->DIER = hw->dier_dma;

    hw->stream->CR |= DMA_SxCR_EN;
    hw->timer->CR1 |= TIM_CR1_CEN;

    transfer_start_msec = HAL_GetTick();
    while ((*hw->dma_isr & hw->dma_tc_flag) == 0U) {
        if ((*hw->dma_isr & hw->dma_error_flags) != 0U) {
            transfer_status = HAL_ERROR;
            break;
        }
        if ((uint32_t)(HAL_GetTick() - transfer_start_msec) >= WS2812_DMA_TIMEOUT_MSEC) {
            transfer_status = HAL_TIMEOUT;
            break;
        }
    }

    /* 타이머와 DMA 를 멈추고 라인을 Low 로 되돌린다. 켜 둔 채로 두면 마지막 듀티가 계속
     * 반복되어 다음 프레임의 첫 비트가 어긋난다. */
    hw->timer->DIER = 0U;
    hw->timer->CR1 &= ~TIM_CR1_CEN;
    *hw->ccr = 0U;
    hw->timer->EGR = TIM_EGR_UG;
    hw->stream->CR &= ~DMA_SxCR_EN;
    *hw->dma_ifcr = hw->dma_clear_flags;

    if (transfer_status == HAL_OK) {
        /* 리셋 구간을 한 번 더 확실히 준다. 연속 호출 사이에 프레임이 붙는 것을 막는다. */
        HAL_Delay(1U);
    }

    return transfer_status;
}

HAL_StatusTypeDef neopixel_clear(neopixel_channel_t channel, uint16_t count)
{
    HAL_StatusTypeDef status = neopixel_fill(channel, count, 0U, 0U, 0U);

    if (status != HAL_OK) {
        return status;
    }
    return neopixel_show(channel, count);
}

HAL_StatusTypeDef neopixel_show_solid(neopixel_channel_t channel, uint16_t count,
                                      uint8_t red, uint8_t green, uint8_t blue)
{
    HAL_StatusTypeDef status = neopixel_fill(channel, count, red, green, blue);

    if (status != HAL_OK) {
        return status;
    }
    return neopixel_show(channel, count);
}
