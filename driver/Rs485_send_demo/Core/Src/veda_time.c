#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

#include "veda_time.h"

static uint32_t ms_clock_last_tick;
static uint32_t ms_clock_wrap_count;

int64_t veda_now_ms(void)
{
  uint32_t now;
  uint32_t wraps;

  taskENTER_CRITICAL();
  now = osKernelGetTickCount();
  if (now < ms_clock_last_tick)
  {
    ++ms_clock_wrap_count;
  }
  ms_clock_last_tick = now;
  wraps = ms_clock_wrap_count;
  taskEXIT_CRITICAL();

  return (int64_t)(((uint64_t)wraps << 32) | (uint64_t)now);
}
