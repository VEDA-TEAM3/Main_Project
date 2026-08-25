#include "driver_protocol.h"
#include "veda_channel.h"
#include "veda_strip.h"

#if MY_SLAVE_ID == 1U
const channel_output_t channel_output[] = {
  { 1U, RELAY_A_GPIO_Port, RELAY_A_Pin, BUZZER_A_GPIO_Port, BUZZER_A_Pin,
    NEOPIXEL_CH_A, "A", "PA0/PB5/PA6" },
  { 2U, RELAY_B_GPIO_Port, RELAY_B_Pin, BUZZER_B_GPIO_Port, BUZZER_B_Pin,
    NEOPIXEL_CH_B, "B", "PA1/PA8/PA7" },
};
#elif MY_SLAVE_ID == 2U
const channel_output_t channel_output[] = {
  { 3U, RELAY_A_GPIO_Port, RELAY_A_Pin, BUZZER_A_GPIO_Port, BUZZER_A_Pin,
    NEOPIXEL_CH_A, "A", "PA0/PB5/PA6" },
  { 4U, RELAY_B_GPIO_Port, RELAY_B_Pin, BUZZER_B_GPIO_Port, BUZZER_B_Pin,
    NEOPIXEL_CH_B, "B", "PA1/PA8/PA7" },
};
#else
#error "이 MY_SLAVE_ID 에 대한 channel_output[] 표가 없다. veda_channel.c 에 추가할 것"
#endif

#define CHANNEL_OUTPUT_COUNT (sizeof(channel_output) / sizeof(channel_output[0]))

_Static_assert(CHANNEL_OUTPUT_COUNT <= CHANNEL_OUTPUT_MAX,
               "channel_output[] 항목 수가 CHANNELS_PER_SLAVE 보다 많다");

const uint8_t channel_output_count = (uint8_t)CHANNEL_OUTPUT_COUNT;

volatile uint8_t channel_risk[CHANNEL_OUTPUT_MAX];

#if BUZZER_ENABLED
#if BUZZER_ACTIVE_HIGH
#define BUZZER_LEVEL_ON  GPIO_PIN_SET
#define BUZZER_LEVEL_OFF GPIO_PIN_RESET
#else
#define BUZZER_LEVEL_ON  GPIO_PIN_RESET
#define BUZZER_LEVEL_OFF GPIO_PIN_SET
#endif
#endif /* BUZZER_ENABLED */

uint8_t channel_output_index(uint8_t channel)
{
  uint8_t index;

  for (index = 0U; index < channel_output_count; ++index)
  {
    if (channel_output[index].channel == channel)
    {
      return index;
    }
  }

  return CHANNEL_OUTPUT_NONE;
}

void channel_hardware_init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  uint8_t index;

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

  for (index = 0U; index < channel_output_count; ++index)
  {
    HAL_GPIO_WritePin(channel_output[index].relay_port, channel_output[index].relay_pin,
                      GPIO_PIN_RESET);
    GPIO_InitStruct.Pin = channel_output[index].relay_pin;
    HAL_GPIO_Init(channel_output[index].relay_port, &GPIO_InitStruct);

#if BUZZER_ENABLED
    HAL_GPIO_WritePin(channel_output[index].buzzer_port, channel_output[index].buzzer_pin,
                      BUZZER_LEVEL_OFF);
    GPIO_InitStruct.Pin = channel_output[index].buzzer_pin;
    HAL_GPIO_Init(channel_output[index].buzzer_port, &GPIO_InitStruct);
#endif
  }
}

void apply_channel_state(uint8_t channel, uint8_t risk_level)
{
  const uint8_t light_on = (uint8_t)((risk_level != (uint8_t)VEDA_RISK_NONE) ? 1U : 0U);
  const uint8_t index = channel_output_index(channel);
  uint8_t scan;
  uint8_t any_on = 0U;
#if BUZZER_ENABLED
  const uint8_t buzzer_on = (uint8_t)((risk_level == (uint8_t)VEDA_RISK_DANGER) ? 1U : 0U);
#endif

  if (index == CHANNEL_OUTPUT_NONE)
  {
    return;
  }

  HAL_GPIO_WritePin(channel_output[index].relay_port, channel_output[index].relay_pin,
                    (light_on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  channel_risk[index] = risk_level;

#if BUZZER_ENABLED
  HAL_GPIO_WritePin(channel_output[index].buzzer_port, channel_output[index].buzzer_pin,
                    (buzzer_on != 0U) ? BUZZER_LEVEL_ON : BUZZER_LEVEL_OFF);
#endif

#if NEOPIXEL_ENABLED
  veda_strip_request(index);
#endif

  for (scan = 0U; scan < channel_output_count; ++scan)
  {
    if (channel_risk[scan] != (uint8_t)VEDA_RISK_NONE)
    {
      any_on = 1U;
    }
  }
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, (any_on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
