#include "cmsis_os.h"

#include "veda_channel.h"
#include "veda_config.h"
#include "veda_rs485.h"
#include "veda_sched.h"
#include "veda_stats.h"
#include "veda_types.h"
#include "veda_uplink.h"

static uint8_t sched_cursor;

#include "ack_match.inc"
#include "sched_select.inc"
#include "sched_dispatch.inc"

void StartSchedTask(void *argument)
{
  uint32_t tick = osKernelGetTickCount();

  (void)argument;

  rs485_de_init();
  veda_rs485_rx_arm();

  for (;;)
  {
    tick += SCHED_TICK_MSEC;
    (void)osDelayUntil(tick);

    ++stat_sched_ticks;

    sched_tick();
  }
}
