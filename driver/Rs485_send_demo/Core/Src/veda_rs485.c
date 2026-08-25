#include "cmsis_os.h"

#include "veda_rs485.h"
#include "veda_stats.h"

uint8_t rs485_rx_byte;

static osMessageQueueId_t ack_queue;
static char ack_line[RS485_ACK_LINE_BUFFER_SIZE];
static uint8_t ack_line_length;
static uint8_t ack_line_overflow;

uint8_t veda_rs485_queue_create(void)
{
  ack_queue = osMessageQueueNew(ACK_QUEUE_DEPTH, sizeof(rs485_ack_t), NULL);
  return (ack_queue != NULL) ? 1U : 0U;
}

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

void veda_rs485_rx_arm(void)
{
  (void)HAL_UART_Receive_IT(&huart1, &rs485_rx_byte, 1U);
}

uint8_t send_channel_command(uint8_t channel_index, uint8_t risk_level)
{
  char frame[RS485_FRAME_BUFFER_SIZE];
  uint8_t length = 0U;
  HAL_StatusTypeDef status;
  const char slave_digit = (char)('1' + (channel_index / CHANNELS_PER_SLAVE));
  const char channel_digit = (char)('1' + channel_index);

  frame[length++] = 'S';
  frame[length++] = slave_digit;
  frame[length++] = ':';
  frame[length++] = 'C';
  frame[length++] = 'H';
  frame[length++] = channel_digit;
  frame[length++] = ':';
  frame[length++] = 'R';
  frame[length++] = (char)('0' + (risk_level % 10U));
  frame[length++] = '\n';

#if RS485_DE_ENABLED
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_SET);
#endif

  status = HAL_UART_Transmit(&huart1, (const uint8_t*)frame, length, RS485_TX_TIMEOUT_MSEC);

#if RS485_DE_ENABLED
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
#endif

  return (status == HAL_OK) ? 1U : 0U;
}

void ack_queue_put(const rs485_ack_t *ack)
{
  (void)osMessageQueuePut(ack_queue, ack, 0U, 0U);
}

uint8_t ack_queue_take(rs485_ack_t *out, uint32_t timeout_ms)
{
  return (osMessageQueueGet(ack_queue, out, NULL, timeout_ms) == osOK) ? 1U : 0U;
}

uint32_t ack_now_ms(void)
{
  return osKernelGetTickCount();
}

#include "ack_parse.inc"

void veda_rs485_ack_feed_byte(uint8_t byte)
{
  ack_feed_byte(byte);
}

void veda_rs485_ack_line_reset(void)
{
  ack_line_length = 0U;
  ack_line_overflow = 0U;
}
