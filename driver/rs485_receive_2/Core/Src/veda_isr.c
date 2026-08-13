#include "veda_isr.h"
#include "veda_rs485.h"

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    process_rs485_byte(*veda_rs485_rx_slot());
    veda_rs485_mark_rx();
    (void)HAL_UART_Receive_IT(huart, veda_rs485_rx_slot(), 1U);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    veda_rs485_line_reset();
    (void)HAL_UART_Receive_IT(huart, veda_rs485_rx_slot(), 1U);
  }
}
