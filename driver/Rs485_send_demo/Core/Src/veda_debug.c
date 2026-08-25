#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "main.h"
#include "veda_debug.h"
#include "veda_tasks.h"

#if MASTER_DEBUG_LOG

extern UART_HandleTypeDef huart2;

void dbg_print(const char *text)
{
  (void)HAL_UART_Transmit(&huart2, (const uint8_t*)text, (uint16_t)strlen(text), 100U);
}

void dbg_print_u32(uint32_t value)
{
  char reversed[10];
  char text[10];
  uint8_t count = 0U;
  uint8_t index;

  do
  {
    reversed[count] = (char)('0' + (uint8_t)(value % 10U));
    ++count;
    value /= 10U;
  } while (value != 0U);

  for (index = 0U; index < count; ++index)
  {
    text[index] = reversed[count - 1U - index];
  }

  (void)HAL_UART_Transmit(&huart2, (const uint8_t*)text, count, 100U);
}

void dbg_print_stat(const char *label, uint32_t value)
{
  dbg_print(label);
  dbg_print("=");
  dbg_print_u32(value);
  dbg_print(" ");
}

static void dbg_print_stack_free(const char *label, osThreadId_t handle)
{
  if (handle == NULL)
  {
    return;
  }
  dbg_print_stat(label, (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)handle) * 4U);
}

void dbg_print_stack_line(void)
{
  dbg_print("[stack] ");
  dbg_print_stack_free("dflt", defaultTaskHandle);
  dbg_print_stack_free("rx", rxTaskHandle);
  dbg_print_stack_free("ctrl", ctrlTaskHandle);
  dbg_print_stack_free("sched", schedTaskHandle);
  dbg_print_stack_free("tx", txTaskHandle);
  dbg_print_stack_free("hb", heartbeatTaskHandle);
  dbg_print("(bytes free)\r\n");
}

#endif /* MASTER_DEBUG_LOG */
