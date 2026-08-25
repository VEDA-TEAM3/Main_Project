#include "veda_stats.h"

volatile uint32_t stat_rx_bytes_dropped;
volatile uint32_t stat_frames_ok;
volatile uint32_t stat_frames_bad;
volatile uint32_t stat_bad_channel;
volatile uint32_t stat_last_bad_channel;
volatile uint32_t stat_bad_risk;
volatile uint32_t stat_last_bad_risk;
volatile uint32_t stat_cmd_dropped;
volatile uint32_t stat_rs485_tx_fail;
volatile uint32_t stat_uplink_tx_fail;
volatile uint32_t stat_uplink_dropped;
volatile uint32_t stat_uplink_sent;
volatile uint32_t stat_ack_ok;
volatile uint32_t stat_ack_bad;
volatile uint32_t stat_ack_timeout;
volatile uint32_t stat_ack_mismatch;
volatile uint32_t stat_cmd_coalesced;
volatile uint32_t stat_sched_retry;
volatile uint32_t stat_lat_max;
volatile uint32_t stat_lat_danger_max;
volatile uint32_t stat_sched_ticks;
volatile uint32_t stat_sched_refresh;
volatile uint32_t last_downlink_tick;
