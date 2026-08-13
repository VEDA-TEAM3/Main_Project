#include "FreeRTOS.h"
#include "task.h"

#include "veda_channel.h"
#include "veda_debug.h"
#include "veda_rs485.h"

static char rs485_line[RS485_LINE_BUFFER_SIZE];
static volatile uint8_t rs485_line_length;
static volatile uint8_t rs485_line_overflow;
static uint8_t rs485_rx_byte;
static volatile uint32_t rs485_last_rx_msec;

#if SLAVE_ACK_ENABLED
static volatile uint8_t ack_pending;
static volatile uint8_t ack_channel;
static volatile uint8_t ack_risk;
#endif

uint8_t process_rs485_line(const char *line, uint8_t length)
{
  uint8_t channel;
  uint8_t risk_level;

  if (length != 9U || line[0] != 'S' || line[2] != ':' ||
      line[3] != 'C' || line[4] != 'H' || line[6] != ':' || line[7] != 'R')
  {
    return VERDICT_BAD_FORMAT;
  }
  if (line[1] < '1' || line[1] > '9' || line[5] < '1' || line[5] > '9')
  {
    return VERDICT_BAD_FORMAT;
  }

  if ((uint8_t)(line[1] - '0') != MY_SLAVE_ID)
  {
    return VERDICT_OTHER_SLAVE;
  }

  if (line[8] < '0' || line[8] > '2')
  {
    return VERDICT_BAD_RISK;
  }
  risk_level = (uint8_t)(line[8] - '0');

  channel = (uint8_t)(line[5] - '0');
  if (channel_output_index(channel) == CHANNEL_OUTPUT_NONE)
  {
    return VERDICT_OTHER_CHAN;
  }

  apply_channel_state(channel, risk_level);

#if SLAVE_ACK_ENABLED
  veda_rs485_ack_offer(channel, risk_level);
#endif

  return VERDICT_APPLIED;
}

void process_rs485_byte(uint8_t byte)
{
  if (byte == '\r' || byte == '\n')
  {
    if (rs485_line_overflow == 0U && rs485_line_length != 0U)
    {
      const uint8_t verdict = process_rs485_line(rs485_line, rs485_line_length);

      veda_debug_line_offer(rs485_line, rs485_line_length, verdict);
      (void)verdict;
    }
    rs485_line_length = 0U;
    rs485_line_overflow = 0U;
    return;
  }

  if (rs485_line_length >= RS485_LINE_BUFFER_SIZE)
  {
    rs485_line_overflow = 1U;
    return;
  }

  rs485_line[rs485_line_length] = (char)byte;
  ++rs485_line_length;
}

void veda_rs485_line_reset(void)
{
  rs485_line_length = 0U;
  rs485_line_overflow = 0U;
}

void veda_rs485_mark_rx(void)
{
  rs485_last_rx_msec = HAL_GetTick();
}

void veda_rs485_idle_service(void)
{
  taskENTER_CRITICAL();
  if ((rs485_line_length != 0U || rs485_line_overflow != 0U) &&
      (HAL_GetTick() - rs485_last_rx_msec) >= RS485_IDLE_RESET_MSEC)
  {
    rs485_line_length = 0U;
    rs485_line_overflow = 0U;
  }
  taskEXIT_CRITICAL();
}

void veda_rs485_rx_arm(void)
{
  (void)HAL_UART_Receive_IT(&huart1, &rs485_rx_byte, 1U);
}

uint8_t *veda_rs485_rx_slot(void)
{
  return &rs485_rx_byte;
}

#if SLAVE_ACK_ENABLED

void rs485_de_init(void)
{
#if RS485_DE_ENABLED
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();

  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = RS485_DE_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(RS485_DE_GPIO_Port, &GPIO_InitStruct);
#endif
}

void veda_rs485_ack_offer(uint8_t channel, uint8_t risk_level)
{
  if (ack_pending == 0U)
  {
    ack_channel = channel;
    ack_risk = risk_level;
    ack_pending = 1U;
  }
}

static void rs485_send_ack(uint8_t channel, uint8_t risk_level)
{
  char frame[RS485_ACK_BUFFER_SIZE];
  uint8_t length = 0U;

  frame[length++] = 'A';
  frame[length++] = (char)('0' + MY_SLAVE_ID);
  frame[length++] = ':';
  frame[length++] = 'C';
  frame[length++] = 'H';
  frame[length++] = (char)('0' + channel);
  frame[length++] = ':';
  frame[length++] = 'R';
  frame[length++] = (char)('0' + (risk_level % 10U));
  frame[length++] = '\n';

#if RS485_DE_ENABLED
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_SET);
#endif

  (void)HAL_UART_Transmit(&huart1, (const uint8_t*)frame, length, RS485_ACK_TX_TIMEOUT_MSEC);

#if RS485_DE_ENABLED
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
#endif
}

void veda_rs485_ack_service(void)
{
  if (ack_pending != 0U)
  {
    const uint8_t channel = ack_channel;
    const uint8_t risk_level = ack_risk;

    rs485_send_ack(channel, risk_level);
    ack_pending = 0U;
  }
}

#endif /* SLAVE_ACK_ENABLED */
