#include <string.h>

#include "cmsis_os.h"

#include "main.h"
#include "veda_config.h"
#include "veda_debug.h"
#include "veda_stats.h"
#include "veda_supervisor.h"
#include "veda_tasks.h"
#include "veda_time.h"
#include "veda_types.h"

#if MASTER_SELFTEST_ENABLED

typedef struct
{
  uint8_t channel_index;
  uint8_t risk_level;
} test_step_t;

static const test_step_t test_sequence[] = {
  { 0U, (uint8_t)VEDA_RISK_WARNING },
  { 0U, (uint8_t)VEDA_RISK_DANGER  },
  { 0U, (uint8_t)VEDA_RISK_NONE    },
  { 1U, (uint8_t)VEDA_RISK_WARNING },
  { 1U, (uint8_t)VEDA_RISK_DANGER  },
  { 1U, (uint8_t)VEDA_RISK_NONE    },
  { 2U, (uint8_t)VEDA_RISK_WARNING },
  { 2U, (uint8_t)VEDA_RISK_DANGER  },
  { 2U, (uint8_t)VEDA_RISK_NONE    },
  { 3U, (uint8_t)VEDA_RISK_WARNING },
  { 3U, (uint8_t)VEDA_RISK_DANGER  },
  { 3U, (uint8_t)VEDA_RISK_NONE    },
};
#define TEST_SEQUENCE_LENGTH (sizeof(test_sequence) / sizeof(test_sequence[0]))

static uint8_t test_step_index;

#endif /* MASTER_SELFTEST_ENABLED */

static void veda_supervisor_banner(void)
{
  dbg_print("\r\n=== VEDA MASTER READY ===\r\n");
  dbg_print(" channels    = ");
  dbg_print_u32(CHANNEL_COUNT);
  dbg_print(" (rpi channel_id base = ");
  dbg_print_u32(RPI_CHANNEL_ID_BASE);
  dbg_print(")\r\n heartbeat   = ");
  dbg_print_u32(HEARTBEAT_INTERVAL_MSEC);
  dbg_print(" ms/channel\r\n downlink    = ");
  dbg_print_u32((uint32_t)sizeof(veda_downlink_frame_t));
  dbg_print("B / uplink = ");
  dbg_print_u32((uint32_t)sizeof(veda_uplink_frame_t));
  dbg_print("B\r\n\r\n");
}

void veda_supervisor_run(void)
{
  uint32_t now = osKernelGetTickCount();
  uint32_t next_log = now + SUPERVISOR_LOG_INTERVAL_MSEC;
  uint32_t next_blink = now;
  uint32_t downlink_age;
  uint32_t last_tick_count = 0U;
#if MASTER_SELFTEST_ENABLED
  uint32_t next_selftest = now + RS485_SEND_INTERVAL_MSEC;
  veda_risk_event_t injected;
#endif

  veda_supervisor_banner();

  for (;;)
  {
    osDelay(SUPERVISOR_TICK_MSEC);
    now = osKernelGetTickCount();

    downlink_age = (last_downlink_tick == 0U) ? LINK_IDLE_WARN_MSEC
                                              : (now - last_downlink_tick);

    if ((now - next_blink) < 0x80000000U)
    {
      HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
      next_blink = now + ((downlink_age >= LINK_IDLE_WARN_MSEC) ? LD2_BLINK_IDLE_MSEC
                                                                : LD2_BLINK_ALIVE_MSEC);
    }

#if MASTER_SELFTEST_ENABLED
    if ((now - next_selftest) < 0x80000000U)
    {
      (void)memset(&injected, 0, sizeof(injected));
      injected.channel_id = (uint8_t)(test_sequence[test_step_index].channel_index + RPI_CHANNEL_ID_BASE);
      injected.risk_level = test_sequence[test_step_index].risk_level;
      injected.timestamp_ms = veda_now_ms();
      injected.dist_mm = VEDA_DIST_MM_NONE;
      (void)veda_tasks_cmd_put(&injected);

#if MASTER_DEBUG_LOG
      {
        const uint8_t step_index = test_sequence[test_step_index].channel_index;

        dbg_print("[selftest] S");
        dbg_print_u32((uint32_t)((step_index / CHANNELS_PER_SLAVE) + 1U));
        dbg_print(":CH");
        dbg_print_u32((uint32_t)(step_index + 1U));
        dbg_print(" -> R");
        dbg_print_u32((uint32_t)test_sequence[test_step_index].risk_level);
        dbg_print("\r\n");
      }
#endif

      ++test_step_index;
      if (test_step_index >= TEST_SEQUENCE_LENGTH)
      {
        test_step_index = 0U;
      }
      next_selftest = now + RS485_SEND_INTERVAL_MSEC;
    }
#endif /* MASTER_SELFTEST_ENABLED */

    if ((now - next_log) < 0x80000000U)
    {
      dbg_print("[stat] ");
      dbg_print_stat("ok", stat_frames_ok);
      dbg_print_stat("bad", stat_frames_bad);
      dbg_print_stat("bad_ch", stat_bad_channel);
      dbg_print_stat("last_bad_ch", stat_last_bad_channel);
      dbg_print_stat("bad_risk", stat_bad_risk);
      dbg_print_stat("last_bad_risk", stat_last_bad_risk);
      dbg_print_stat("rx_drop", stat_rx_bytes_dropped);
      dbg_print_stat("cmd_drop", stat_cmd_dropped);
      dbg_print_stat("coalesce", stat_cmd_coalesced);
      dbg_print_stat("retry", stat_sched_retry);
      dbg_print_stat("refresh", stat_sched_refresh);
      dbg_print_stat("lat_max", stat_lat_max);
      dbg_print_stat("lat_dgr", stat_lat_danger_max);
      {
        const uint32_t ticks_now = stat_sched_ticks;

        dbg_print_stat("tick5s", ticks_now - last_tick_count);
        last_tick_count = ticks_now;
      }
      dbg_print_stat("485_fail", stat_rs485_tx_fail);
      dbg_print_stat("ack_ok", stat_ack_ok);
      dbg_print_stat("ack_bad", stat_ack_bad);
      dbg_print_stat("ack_to", stat_ack_timeout);
      dbg_print_stat("ack_mis", stat_ack_mismatch);
      dbg_print_stat("up_tx", stat_uplink_sent);
      dbg_print_stat("up_fail", stat_uplink_tx_fail);
      dbg_print_stat("up_drop", stat_uplink_dropped);
      dbg_print("dl_age=");
      if (last_downlink_tick == 0U)
      {
        dbg_print("never");
      }
      else
      {
        dbg_print_u32(downlink_age);
        dbg_print("ms");
      }
      dbg_print("\r\n");

      dbg_print_stack_line();

      next_log = now + SUPERVISOR_LOG_INTERVAL_MSEC;
    }
  }
}
