#include <string.h>

#include "cmsis_os.h"

#include "veda_channel.h"
#include "veda_config.h"
#include "veda_rpi.h"
#include "veda_stats.h"
#include "veda_time.h"
#include "veda_uplink.h"

static osMessageQueueId_t uplink_queue;

uint8_t veda_uplink_queue_create(void)
{
  uplink_queue = osMessageQueueNew(UPLINK_QUEUE_DEPTH, sizeof(veda_uplink_packet_t), NULL);
  return (uplink_queue != NULL) ? 1U : 0U;
}

void enqueue_uplink(uint8_t channel_index, uint8_t reason)
{
  veda_uplink_packet_t packet;
  channel_status_t status;

  (void)memset(&packet, 0, sizeof(packet));

  (void)osMutexAcquire(channel_status_mutex, osWaitForever);
  status = channel_status[channel_index];
  (void)osMutexRelease(channel_status_mutex);

  packet.channel_id = (uint8_t)(channel_index + RPI_CHANNEL_ID_BASE);
  packet.reason = reason;
  packet.siren_on = status.siren_on;
  packet.buzzer_on = status.buzzer_on;
  packet.led_red = status.led_red;
  packet.led_yellow = status.led_yellow;
  packet.led_green = status.led_green;
  packet.timestamp_ms = veda_now_ms();

  if (osMessageQueuePut(uplink_queue, &packet, 0U, 0U) != osOK)
  {
    ++stat_uplink_dropped;
  }
}

void uplink_send(const veda_uplink_packet_t *packet)
{
  veda_uplink_frame_t frame;

  frame.start_byte = VEDA_START_BYTE;
  frame.payload = *packet;
  frame.checksum = veda_uplink_checksum(&frame.payload);
  frame.end_byte = VEDA_END_BYTE;

  if (HAL_UART_Transmit(&huart6, (const uint8_t*)&frame, (uint16_t)sizeof(frame),
                        UPLINK_TX_TIMEOUT_MSEC) == HAL_OK)
  {
    ++stat_uplink_sent;
  }
  else
  {
    ++stat_uplink_tx_fail;
  }
}

uint8_t veda_uplink_queue_get(veda_uplink_packet_t *out)
{
  return (osMessageQueueGet(uplink_queue, out, NULL, osWaitForever) == osOK) ? 1U : 0U;
}
