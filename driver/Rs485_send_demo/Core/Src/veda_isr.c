#include "veda_isr.h"
#include "veda_downlink.h"
#include "veda_rpi.h"
#include "veda_rs485.h"
#include "veda_stats.h"

static uint8_t rpi_rx_byte;

void veda_downlink_rx_arm(void)
{
  (void)HAL_UART_Receive_IT(&huart6, &rpi_rx_byte, 1U);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART6)
  {
    if (veda_downlink_queue_put_from_isr(rpi_rx_byte) == 0U)
    {
      ++stat_rx_bytes_dropped;
    }

    (void)HAL_UART_Receive_IT(huart, &rpi_rx_byte, 1U);
  }
  else if (huart->Instance == USART1)
  {
    veda_rs485_ack_feed_byte(rs485_rx_byte);

    (void)HAL_UART_Receive_IT(huart, &rs485_rx_byte, 1U);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART6)
  {
    (void)HAL_UART_Receive_IT(huart, &rpi_rx_byte, 1U);
  }
  else if (huart->Instance == USART1)
  {
    veda_rs485_ack_line_reset();

    (void)HAL_UART_Receive_IT(huart, &rs485_rx_byte, 1U);
  }
}
