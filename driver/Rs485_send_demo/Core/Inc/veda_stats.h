/**
 * @file veda_stats.h
 * @brief 진단 카운터 전부. 감시 태스크가 5초마다 USART2 로 찍는 `[stat]` 한 줄의 원본이다.
 *
 * 한곳에 모은 이유: 이 카운터들은 여러 모듈(ISR, rx_task, ctrl_task, sched_task, tx_task)이
 * 나눠 올리고 감시 태스크 하나가 모아 찍는다. 각 모듈에 흩어 두면 "지금 무엇을 재고 있는지"의
 * 전체 그림이 사라지고, 이 시스템에서 그 그림은 실기 진단의 유일한 창이다.
 *
 * 전부 volatile 이다. ISR 문맥에서 올라가는 것이 섞여 있고, 감시 태스크는 그것을 읽기만 한다.
 * 원자적 증가를 보장하지 않는 것은 의도적이다 -- 진단 수치이지 제어 입력이 아니므로 몇 개
 * 어긋나도 판단이 달라지지 않고, 여기에 락을 들이면 실시간 경로가 진단 때문에 느려진다.
 *
 * ### 실기에서 볼 것
 *
 * | 값 | 정상 | 이상하면 |
 * |---|---|---|
 * | `ack_ok`   | 계속 증가 | 0에서 안 움직이면 회선이 반이중으로 동작하지 않는다 |
 * | `ack_to` / `retry` | 0 근처 | 계속 오르면 그 채널의 Slave가 응답하지 않는다 |
 * | `lat_dgr`  | 36ms 근처 | 218ms 는 보드 한 대가 죽었을 때의 값이다 |
 * | `tick5s`   | 250 (5초 / 20ms) | 적으면 osDelayUntil 이 밀리는 것 |
 * | `bad_ch` / `last_bad_ch` | 0 | RPI_CHANNEL_ID_BASE 가 RPi 와 어긋났다 |
 */

#ifndef VEDA_STATS_H
#define VEDA_STATS_H

#include <stdint.h>

extern volatile uint32_t stat_rx_bytes_dropped;  /**< 바이트 큐가 가득 차 ISR이 버린 수 */
extern volatile uint32_t stat_frames_ok;         /**< 체크섬까지 통과한 하행 프레임 수 */
extern volatile uint32_t stat_frames_bad;        /**< 체크섬/END 불일치 또는 중간에 끊긴 프레임 수 */
extern volatile uint32_t stat_bad_channel;       /**< channel_id가 범위 밖이라 버린 이벤트 수 */
extern volatile uint32_t stat_last_bad_channel;  /**< 마지막으로 거부한 channel_id (base 오설정 진단용) */
extern volatile uint32_t stat_bad_risk;          /**< risk_level이 0/1/2가 아니라 버린 이벤트 수 */
extern volatile uint32_t stat_last_bad_risk;     /**< 마지막으로 거부한 risk_level 값 */
extern volatile uint32_t stat_cmd_dropped;       /**< ctrl_task가 밀려 버린 이벤트 수 */
extern volatile uint32_t stat_rs485_tx_fail;     /**< RS-485 송신 실패 수 */
extern volatile uint32_t stat_uplink_tx_fail;    /**< 상행 송신 실패 수 */
extern volatile uint32_t stat_uplink_dropped;    /**< 상행 큐가 가득 차 버린 패킷 수 */
extern volatile uint32_t stat_uplink_sent;       /**< 상행으로 내보낸 프레임 수 */
extern volatile uint32_t stat_ack_ok;            /**< 형식이 맞는 ACK를 Slave에게서 받은 수 */
extern volatile uint32_t stat_ack_bad;           /**< 형식이 어긋나 버린 ACK 줄 수 */
extern volatile uint32_t stat_ack_timeout;       /**< 기다렸는데 ACK가 오지 않은 명령 수 */
extern volatile uint32_t stat_ack_mismatch;      /**< ACK는 왔지만 보낸 명령과 내용이 다른 수 */

/**
 * desired 와 같은 등급이 다시 와서 회선에 내보내지 않은 이벤트 수.
 *
 * 예전 구조에서 그대로 버스에 나가 백로그를 만들던 것이 이 값이다. 크게 올라간다면
 * RPi가 같은 등급을 반복 송신 중이라는 뜻이고, 그것이 정상이다(스케줄러가 흡수한다).
 */
extern volatile uint32_t stat_cmd_coalesced;

/** sched_task 가 재전송한 횟수(송신 실패 또는 ACK 타임아웃 뒤 다시 시도한 수) */
extern volatile uint32_t stat_sched_retry;

/**
 * 명령이 Slave까지 확인되기까지 걸린 시간의 최댓값(ms). 부팅 이후 누적이다.
 *
 * lat_dgr 이 이 시스템의 반응 상한이다 -- DANGER 는 경보를 켜는 명령이라 이 값만이
 * "경광등이 최악 몇 ms 안에 켜지는가"에 답한다. 설계상 예상치는 다음과 같다:
 *   정상    : 한 틱(20ms) + 채널 순번 대기. 4채널이므로 최악 80ms 근처.
 *   고장 중 : 앞 채널이 ACK 타임아웃(100ms)을 먹으면 그만큼 밀린다.
 * lat_max 는 등급을 가리지 않은 전체 최댓값이라 둘을 비교하면 DANGER 우선 패스가
 * 실제로 먼저 나가고 있는지 확인된다(lat_dgr 이 lat_max 보다 작아야 정상이다).
 */
extern volatile uint32_t stat_lat_max;
extern volatile uint32_t stat_lat_danger_max;

/**
 * sched_task 루프를 돈 횟수. 틱 주기가 진짜 SCHED_TICK_MSEC 인지 확인하는 유일한 수단이다.
 *
 * 왜 필요한가: 지금까지 재고 못박은 모든 지연 숫자(lat_max / lat_dgr / 호스트 테스트 상한)가
 * "틱이 정말 20ms 다"라는 전제 위에 있다. osDelayUntil 의 기준선이 밀리거나 우선순위가
 * 뒤엉켜 틱이 느려지면 그 숫자들이 통째로 무의미해지는데, 밖에서는 전혀 보이지 않는다.
 *
 * 감시 태스크가 5초마다 증가분을 찍는다(tick5s). 기대값은 5000 / 20 = 250 회.
 *  - 250 근처   : 정상
 *  - 250 보다 적음 : osDelayUntil 기준선이 밀리고 있다(틱이 느리다)
 *  - 250 보다 많음 : ACK 타임아웃으로 백투백이 돌았다(고장 중에는 정상이다)
 */
extern volatile uint32_t stat_sched_ticks;

/** 값이 그대로인데 주기가 되어 다시 내보낸 수 (SCHED_REFRESH_MSEC) */
extern volatile uint32_t stat_sched_refresh;

/** 마지막으로 유효한 하행 프레임을 받은 시각. 0이면 부팅 후 한 번도 받지 못한 것이다. */
extern volatile uint32_t last_downlink_tick;

#endif /* VEDA_STATS_H */
