#include <string.h>

#include "main.h"
#include "veda_debug.h"
#include "veda_rs485.h"

#if SLAVE_DEBUG_LOG

extern UART_HandleTypeDef huart2;

static const char *const verdict_text[VERDICT_COUNT] = {
  "APPLIED (relay+buzzer+strip)",
  "dropped: bad format",
  "dropped: another slave",
  "dropped: bad risk level",
  "dropped: not my channel",
};

static char debug_line[RS485_LINE_BUFFER_SIZE + 1U];
static volatile uint8_t debug_verdict;
static volatile uint8_t debug_ready;

static uint32_t loop_count;
static uint32_t loop_report_msec;

void slave_print(const char *text)
{
  (void)HAL_UART_Transmit(&huart2, (const uint8_t*)text, (uint16_t)strlen(text), 100U);
}

void slave_print_digit(uint8_t value)
{
  const char text[2] = { (char)('0' + (value % 10U)), '\0' };
  slave_print(text);
}

void slave_print_u32(uint32_t value)
{
  char reversed[10];
  char text[11];
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
  text[count] = '\0';

  slave_print(text);
}

void veda_debug_line_offer(const char *line, uint8_t length, uint8_t verdict)
{
  if (debug_ready != 0U || length > RS485_LINE_BUFFER_SIZE)
  {
    return;
  }

  (void)memcpy(debug_line, line, length);
  debug_line[length] = '\0';
  debug_verdict = verdict;
  debug_ready = 1U;
}

void veda_debug_line_service(void)
{
  if (debug_ready == 0U)
  {
    return;
  }

  slave_print("RX \"");
  slave_print(debug_line);
  slave_print("\" -> ");
  if (debug_verdict < VERDICT_COUNT)
  {
    slave_print(verdict_text[debug_verdict]);
  }
  slave_print("\r\n");
  debug_ready = 0U;
}

void veda_debug_loop_begin(void)
{
  loop_count = 0U;
  loop_report_msec = HAL_GetTick();
}

void veda_debug_loop_tick(void)
{
  ++loop_count;

  if ((HAL_GetTick() - loop_report_msec) >= 5000U)
  {
    const uint32_t elapsed = HAL_GetTick() - loop_report_msec;

    slave_print("[loop] ");
    slave_print_u32(loop_count);
    slave_print(" turns / 5s -> ");
    slave_print_u32((loop_count != 0U) ? (elapsed / loop_count) : 0U);
    slave_print("ms each\r\n");

    loop_count = 0U;
    loop_report_msec = HAL_GetTick();
  }
}

#endif /* SLAVE_DEBUG_LOG */
