#include <string.h>

#include "veda_channel.h"

channel_status_t channel_status[CHANNEL_COUNT];
channel_ctrl_t channel_ctrl[CHANNEL_COUNT];
osMutexId_t channel_status_mutex;

static const osMutexAttr_t channel_status_mutex_attributes = {
  .name = "channelStatusMutex",
};

uint8_t veda_channel_mutex_create(void)
{
  channel_status_mutex = osMutexNew(&channel_status_mutex_attributes);
  return (channel_status_mutex != NULL) ? 1U : 0U;
}

void risk_to_status(uint8_t risk_level, channel_status_t *out)
{
  out->risk_level = risk_level;

  switch (risk_level)
  {
    case (uint8_t)VEDA_RISK_DANGER:
      out->siren_on = 1U;
      out->led_red = 1U;
      out->led_yellow = 0U;
      out->led_green = 0U;
      break;

    case (uint8_t)VEDA_RISK_WARNING:
      out->siren_on = 1U;
      out->led_red = 0U;
      out->led_yellow = 1U;
      out->led_green = 0U;
      break;

    case (uint8_t)VEDA_RISK_NONE:
    default:
      out->risk_level = (uint8_t)VEDA_RISK_NONE;
      out->siren_on = 0U;
      out->led_red = 0U;
      out->led_yellow = 0U;
      out->led_green = 1U;
      break;
  }

  out->buzzer_on = (uint8_t)((out->risk_level == (uint8_t)VEDA_RISK_DANGER) ? 1U : 0U);
}

void channel_boot_state_init(void)
{
  uint8_t index;

  (void)memset(channel_status, 0, sizeof(channel_status));

  for (index = 0U; index < CHANNEL_COUNT; ++index)
  {
    channel_ctrl[index].applied_risk = SCHED_RISK_UNKNOWN;
  }
}
