#include "cmsis_os.h"

#include "driver_protocol.h"
#include "main.h"
#include "neopixel.h"
#include "veda_channel.h"
#include "veda_config.h"
#include "veda_debug.h"
#include "veda_rs485.h"
#include "veda_strip.h"
#include "veda_supervisor.h"

static void veda_supervisor_blink_id(void)
{
  uint8_t blink;

  for (blink = 0U; blink < MY_SLAVE_ID; ++blink)
  {
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
    osDelay(SLAVE_ID_BLINK_MSEC);
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
    osDelay(SLAVE_ID_BLINK_MSEC);
  }
}

#if SLAVE_DEBUG_LOG
static void veda_supervisor_banner(void)
{
  uint8_t index;

  slave_print("\r\nSLAVE #");
  slave_print_digit(MY_SLAVE_ID);
  slave_print(" READY - owns ");

  for (index = 0U; index < channel_output_count; ++index)
  {
    if (index != 0U)
    {
      slave_print(", ");
    }
    slave_print("CH");
    slave_print_digit(channel_output[index].channel);
    slave_print("=");
    slave_print(channel_output[index].set_name);
    slave_print("(");
    slave_print(channel_output[index].wiring);
    slave_print(")");

    if ((channel_output[index].channel < MY_FIRST_CHANNEL) ||
        (channel_output[index].channel > MY_LAST_CHANNEL))
    {
      slave_print("!!OUT-OF-RANGE");
    }
  }

#if BUZZER_ENABLED
  slave_print(", buzzer=");
  slave_print(BUZZER_ACTIVE_HIGH ? "active-high" : "active-low");
#endif

#if NEOPIXEL_ENABLED
  slave_print(", strip x");
  slave_print_digit((uint8_t)((NEOPIXEL_PIXEL_COUNT / 10U) % 10U));
  slave_print_digit((uint8_t)(NEOPIXEL_PIXEL_COUNT % 10U));
  slave_print("/ch");
#if NEOPIXEL_SELFTEST_ENABLED
  slave_print(" !!SELFTEST");
#endif
#endif

  slave_print("\r\n");
}
#else
#define veda_supervisor_banner() ((void)0)
#endif /* SLAVE_DEBUG_LOG */

#if NEOPIXEL_ENABLED
static void veda_supervisor_strip_boot(void)
{
  uint8_t index;

  for (index = 0U; index < channel_output_count; ++index)
  {
    if (neopixel_init(channel_output[index].strip) != HAL_OK)
    {
#if SLAVE_DEBUG_LOG
      slave_print("!! neopixel_init FAILED on ch ");
      slave_print(channel_output[index].set_name);
      slave_print(" - timer clock is not 84MHz\r\n");
#endif
      continue;
    }

    if (strip_show(index, (uint8_t)VEDA_RISK_NONE) == HAL_OK)
    {
      veda_strip_mark_shown(index, (uint8_t)VEDA_RISK_NONE);
    }
  }
}
#endif /* NEOPIXEL_ENABLED */

void veda_supervisor_run(void)
{
#if (NEOPIXEL_ENABLED && NEOPIXEL_SELFTEST_ENABLED)
  uint8_t selftest_risk = (uint8_t)VEDA_RISK_NONE;
  uint32_t selftest_last_msec = HAL_GetTick();
  uint8_t index;
#endif

  veda_supervisor_blink_id();
  veda_supervisor_banner();

#if NEOPIXEL_ENABLED
  veda_supervisor_strip_boot();
#endif

#if SLAVE_ACK_ENABLED
  rs485_de_init();
#endif

  veda_debug_loop_begin();

  veda_rs485_rx_arm();

  for (;;)
  {
#if SLAVE_ACK_ENABLED
    veda_rs485_ack_service();
#endif

    veda_debug_line_service();

#if NEOPIXEL_ENABLED
#if NEOPIXEL_SELFTEST_ENABLED
    if ((HAL_GetTick() - selftest_last_msec) >= NEOPIXEL_SELFTEST_STEP_MSEC)
    {
      selftest_last_msec = HAL_GetTick();

      for (index = 0U; index < channel_output_count; ++index)
      {
        (void)strip_show(index, selftest_risk);
      }

      selftest_risk = (uint8_t)((selftest_risk >= (uint8_t)VEDA_RISK_DANGER)
                                  ? (uint8_t)VEDA_RISK_NONE
                                  : (uint8_t)(selftest_risk + 1U));
    }
#else
    neopixel_service();
#endif
#endif

    veda_rs485_idle_service();

    veda_debug_loop_tick();

    osDelay(10);
  }
}
