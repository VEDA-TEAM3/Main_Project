#include "driver_protocol.h"
#include "neopixel.h"
#include "veda_channel.h"
#include "veda_debug.h"
#include "veda_strip.h"

#if NEOPIXEL_ENABLED

static volatile uint8_t strip_update_pending[CHANNEL_OUTPUT_MAX];
static uint8_t strip_shown_risk[CHANNEL_OUTPUT_MAX];

HAL_StatusTypeDef strip_show(uint8_t index, uint8_t risk_level)
{
  const neopixel_channel_t strip = channel_output[index].strip;
  HAL_StatusTypeDef status;
  uint8_t red;
  uint8_t green;
  uint8_t blue;

  switch (risk_level)
  {
    case (uint8_t)VEDA_RISK_NONE:
      red = NEOPIXEL_NONE_R;    green = NEOPIXEL_NONE_G;    blue = NEOPIXEL_NONE_B;
      break;

    case (uint8_t)VEDA_RISK_WARNING:
      red = NEOPIXEL_WARNING_R; green = NEOPIXEL_WARNING_G; blue = NEOPIXEL_WARNING_B;
      break;

    case (uint8_t)VEDA_RISK_DANGER:
      red = NEOPIXEL_DANGER_R;  green = NEOPIXEL_DANGER_G;  blue = NEOPIXEL_DANGER_B;
      break;

    default:
      return HAL_ERROR;
  }

  status = neopixel_show_solid(strip, NEOPIXEL_PIXEL_COUNT, red, green, blue);

#if SLAVE_DEBUG_LOG
  if (status != HAL_OK)
  {
    slave_print("!! strip ");
    slave_print(channel_output[index].set_name);
    slave_print((status == HAL_TIMEOUT) ? " DMA timeout\r\n" : " show error\r\n");
  }
#endif

  return status;
}

void veda_strip_mark_shown(uint8_t index, uint8_t risk_level)
{
  strip_shown_risk[index] = risk_level;
}

void veda_strip_request(uint8_t index)
{
  strip_update_pending[index] = 1U;
}

#if !NEOPIXEL_SELFTEST_ENABLED
void neopixel_service(void)
{
  uint8_t index;

  for (index = 0U; index < channel_output_count; ++index)
  {
    uint8_t risk;

    if (strip_update_pending[index] == 0U)
    {
      continue;
    }

    strip_update_pending[index] = 0U;
    risk = channel_risk[index];

    if (risk == strip_shown_risk[index])
    {
      continue;
    }

    if (strip_show(index, risk) == HAL_OK)
    {
      strip_shown_risk[index] = risk;
    }
  }
}
#endif /* !NEOPIXEL_SELFTEST_ENABLED */

#endif /* NEOPIXEL_ENABLED */
