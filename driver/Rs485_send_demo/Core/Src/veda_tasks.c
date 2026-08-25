#include "cmsis_os.h"

#include "veda_channel.h"
#include "veda_config.h"
#include "veda_downlink.h"
#include "veda_isr.h"
#include "veda_sched.h"
#include "veda_stats.h"
#include "veda_tasks.h"
#include "veda_types.h"
#include "veda_uplink.h"

osThreadId_t rxTaskHandle;
osThreadId_t ctrlTaskHandle;
osThreadId_t schedTaskHandle;
osThreadId_t txTaskHandle;
osThreadId_t heartbeatTaskHandle;

static osMessageQueueId_t cmd_queue;

static const osThreadAttr_t rxTask_attributes = {
  .name = "rxTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
static const osThreadAttr_t ctrlTask_attributes = {
  .name = "ctrlTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
static const osThreadAttr_t schedTask_attributes = {
  .name = "schedTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
static const osThreadAttr_t txTask_attributes = {
  .name = "txTask",
  .stack_size = 192 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
static const osThreadAttr_t heartbeatTask_attributes = {
  .name = "hbTask",
  .stack_size = 192 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};

uint8_t veda_tasks_queue_create(void)
{
  cmd_queue = osMessageQueueNew(CMD_QUEUE_DEPTH, sizeof(veda_risk_event_t), NULL);
  return (cmd_queue != NULL) ? 1U : 0U;
}

uint8_t veda_tasks_cmd_put(const void *event)
{
  return (osMessageQueuePut(cmd_queue, event, 0U, 0U) == osOK) ? 1U : 0U;
}

uint8_t veda_tasks_create(void)
{
  rxTaskHandle = osThreadNew(StartRxTask, NULL, &rxTask_attributes);
  ctrlTaskHandle = osThreadNew(StartCtrlTask, NULL, &ctrlTask_attributes);
  schedTaskHandle = osThreadNew(StartSchedTask, NULL, &schedTask_attributes);
  txTaskHandle = osThreadNew(StartTxTask, NULL, &txTask_attributes);
  heartbeatTaskHandle = osThreadNew(StartHeartbeatTask, NULL, &heartbeatTask_attributes);

  if ((rxTaskHandle == NULL) || (ctrlTaskHandle == NULL) || (schedTaskHandle == NULL) ||
      (txTaskHandle == NULL) || (heartbeatTaskHandle == NULL))
  {
    return 0U;
  }
  return 1U;
}

void StartRxTask(void *argument)
{
  uint8_t byte;
  veda_risk_event_t event;

  (void)argument;

  veda_downlink_rx_arm();

  for (;;)
  {
    if (veda_downlink_queue_get(&byte, DOWNLINK_RESYNC_MSEC) != 0U)
    {
      if (downlink_feed_byte(byte, &event) != 0U)
      {
        ++stat_frames_ok;
        last_downlink_tick = osKernelGetTickCount();

        if (veda_tasks_cmd_put(&event) == 0U)
        {
          ++stat_cmd_dropped;
        }
      }
    }
    else if (downlink_state != DL_WAIT_START)
    {
      veda_downlink_resync();
    }
    else
    {
    }
  }
}

void StartCtrlTask(void *argument)
{
  veda_risk_event_t event;
  channel_status_t next;
  uint32_t channel_offset;
  uint8_t channel_index;

  (void)argument;

  for (;;)
  {
    if (osMessageQueueGet(cmd_queue, &event, NULL, osWaitForever) != osOK)
    {
      continue;
    }

    channel_offset = (uint32_t)event.channel_id - (uint32_t)RPI_CHANNEL_ID_BASE;
    if (channel_offset >= CHANNEL_COUNT)
    {
      ++stat_bad_channel;
      stat_last_bad_channel = event.channel_id;
      continue;
    }
    channel_index = (uint8_t)channel_offset;

    if ((event.risk_level != (uint8_t)VEDA_RISK_NONE) &&
        (event.risk_level != (uint8_t)VEDA_RISK_WARNING) &&
        (event.risk_level != (uint8_t)VEDA_RISK_DANGER))
    {
      ++stat_bad_risk;
      stat_last_bad_risk = event.risk_level;
    }

    risk_to_status(event.risk_level, &next);

    if (next.risk_level == channel_ctrl[channel_index].desired_risk)
    {
      ++stat_cmd_coalesced;
    }
    else if (next.risk_level == channel_ctrl[channel_index].applied_risk)
    {
      channel_ctrl[channel_index].lat_pending = 0U;
    }
    else
    {
      channel_ctrl[channel_index].desired_tick = osKernelGetTickCount();
      channel_ctrl[channel_index].lat_risk = next.risk_level;
      channel_ctrl[channel_index].lat_pending = 1U;
    }

    channel_ctrl[channel_index].desired_risk = next.risk_level;
  }
}

void StartTxTask(void *argument)
{
  veda_uplink_packet_t packet;

  (void)argument;

  for (;;)
  {
    if (veda_uplink_queue_get(&packet) != 0U)
    {
      uplink_send(&packet);
    }
  }
}

void StartHeartbeatTask(void *argument)
{
  uint32_t next_wake = osKernelGetTickCount();
  uint8_t channel_index;

  (void)argument;

  for (;;)
  {
    next_wake += HEARTBEAT_INTERVAL_MSEC;
    (void)osDelayUntil(next_wake);

    for (channel_index = 0U; channel_index < CHANNEL_COUNT; ++channel_index)
    {
      enqueue_uplink(channel_index, (uint8_t)VEDA_UPLINK_REASON_HEARTBEAT);
    }
  }
}
