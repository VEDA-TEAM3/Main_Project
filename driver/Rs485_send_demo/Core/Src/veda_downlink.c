#include <string.h>

#include "cmsis_os.h"

#include "veda_config.h"
#include "veda_downlink.h"
#include "veda_stats.h"

downlink_state_t downlink_state;

static osMessageQueueId_t rpi_rx_queue;
static uint8_t downlink_payload[sizeof(veda_risk_event_t)];
static uint8_t downlink_payload_index;
static uint8_t downlink_rx_checksum;

uint8_t veda_downlink_queue_create(void)
{
  rpi_rx_queue = osMessageQueueNew(RPI_RX_QUEUE_DEPTH, sizeof(uint8_t), NULL);
  return (rpi_rx_queue != NULL) ? 1U : 0U;
}

uint8_t veda_downlink_queue_put_from_isr(uint8_t byte)
{
  return (osMessageQueuePut(rpi_rx_queue, &byte, 0U, 0U) == osOK) ? 1U : 0U;
}

uint8_t veda_downlink_queue_get(uint8_t *out, uint32_t timeout_ms)
{
  return (osMessageQueueGet(rpi_rx_queue, out, NULL, timeout_ms) == osOK) ? 1U : 0U;
}

uint8_t downlink_feed_byte(uint8_t byte, veda_risk_event_t *out)
{
  uint8_t accepted = 0U;

  switch (downlink_state)
  {
    case DL_WAIT_START:
      if (byte == VEDA_START_BYTE)
      {
        downlink_payload_index = 0U;
        downlink_state = DL_READ_PAYLOAD;
      }
      break;

    case DL_READ_PAYLOAD:
      downlink_payload[downlink_payload_index] = byte;
      ++downlink_payload_index;
      if (downlink_payload_index == (uint8_t)sizeof(downlink_payload))
      {
        downlink_state = DL_READ_CHECKSUM;
      }
      break;

    case DL_READ_CHECKSUM:
      downlink_rx_checksum = byte;
      downlink_state = DL_WAIT_END;
      break;

    case DL_WAIT_END:
    default:
      downlink_state = DL_WAIT_START;
      if ((byte == VEDA_END_BYTE) &&
          (veda_checksum(downlink_payload, sizeof(downlink_payload)) == downlink_rx_checksum))
      {
        (void)memcpy(out, downlink_payload, sizeof(*out));
        accepted = 1U;
      }
      else
      {
        ++stat_frames_bad;
      }
      break;
  }

  return accepted;
}

void veda_downlink_resync(void)
{
  downlink_state = DL_WAIT_START;
  ++stat_frames_bad;
}
