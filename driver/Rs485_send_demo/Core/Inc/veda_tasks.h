/**
 * @file veda_tasks.h
 * @brief 태스크 넷(rx / ctrl / tx / hb)과 태스크 생성. 스케줄러 태스크는 veda_sched.h 에 있다.
 *
 * ### 하행 한 장이 경광등까지 가는 경로
 * ```
 * USART6 ISR --(바이트)--> rx_task --(veda_risk_event_t)--> ctrl_task --> desired 갱신
 *                                                                 |
 *                                              sched_task --------+--> RS-485 --> Slave
 *                                                                 |
 *                              hb_task(500ms 주기) ---------+-----+
 *                                                           v
 *                                               tx_task --> USART6 상행
 * ```
 *
 * ### 왜 단계마다 큐로 끊었는가
 *  - ISR을 짧게 유지해야 오버런이 나지 않는다(veda_downlink.h 참고).
 *  - RS-485 송신(블로킹, 약 1ms)이 도는 동안에도 하행 수신은 계속 흘러야 한다.
 *  - HEARTBEAT는 하행 처리 상태와 무관하게 일정 주기로 나가야 한다. 같은 태스크에 두면
 *    하행이 몰릴 때 주기가 밀리고, 그대로 RPi의 dead 오판으로 이어진다.
 *  - USART6 송신자를 tx_task 하나로 묶어야 ACK와 HEARTBEAT가 섞이지 않는다.
 *
 * ### 우선순위
 * | 태스크 | 우선순위 | 스택 | 이유 |
 * |---|---|---|---|
 * | rxTask    | AboveNormal | 1024B | 바이트 큐를 가장 먼저 비워야 오버런이 나지 않는다 |
 * | ctrlTask  | Normal      | 1024B | 접수. sched 와 같은 우선순위 |
 * | schedTask | Normal      | 1024B | 유일한 RS-485 송신자. 접수와 실행 중 어느 쪽도 상대를 굶기면 안 된다 |
 * | txTask    | Normal      | 768B  | 상행 송신 |
 * | hbTask    | BelowNormal | 768B  | 주기 보고. 밀려도 되지만 밀린 만큼 RPi의 dead 판정에 가까워진다 |
 *
 * 스택은 바이트 단위다(CMSIS-RTOS2 규약: `stack_size = 워드수 * 4`).
 * 실제 여유는 `[stack]` 로그로 확인한다 -- 100바이트 아래면 키울 것.
 */

#ifndef VEDA_TASKS_H
#define VEDA_TASKS_H

#include <stdint.h>

#include "cmsis_os.h"

/* 진단 로그(dbg_print_stack_line)가 태스크별 스택 여유를 찍으려면 핸들이 필요하다. */
extern osThreadId_t defaultTaskHandle;   /**< main.c(CubeMX)가 정의한다. 감시 태스크. */
extern osThreadId_t rxTaskHandle;
extern osThreadId_t ctrlTaskHandle;
extern osThreadId_t schedTaskHandle;
extern osThreadId_t txTaskHandle;
extern osThreadId_t heartbeatTaskHandle;

/**
 * @brief ctrl_task 로 가는 명령 큐(cmd_queue)를 만든다. osKernelInitialize() 뒤에 부를 것.
 * @retval 1 성공, 0 실패 (힙 부족)
 */
uint8_t veda_tasks_queue_create(void);

/**
 * @brief 검증된 위험 이벤트 한 장을 ctrl_task 로 넘긴다.
 * @retval 1 넣었다, 0 큐가 가득 찼다(호출자가 stat_cmd_dropped 를 올린다)
 * @details rx_task 가 부르고, MASTER_SELFTEST_ENABLED 일 때는 감시 태스크도 같은 큐로 넣는다
 * -- 그래야 ctrl_task 이후가 운영 경로와 완전히 동일해진다.
 */
uint8_t veda_tasks_cmd_put(const void *event);

/**
 * @brief 태스크 다섯 개(rx / ctrl / sched / tx / hb)를 만든다.
 * @retval 1 전부 성공, 0 하나라도 실패
 * @details **다섯 개를 모두 검사해야 한다.** schedTaskHandle 이 특히 위험하다: sched_task 는
 * 유일한 RS-485 송신자이고 rs485_de_init() 과 ACK 수신 개방까지 그 안에 있다. 생성에
 * 실패해도 나머지 태스크는 정상 동작하므로 시스템은 멀쩡히 부팅하고, hb_task 가 부팅값을
 * 계속 올려 보내 RPi 는 Master 가 건강하다고 판단한다 -- 경보가 영원히 울리지 않는데
 * 어디에도 에러가 남지 않는다. ACK 검증 기능 전체가 막으려던 바로 그 침묵 고장이다.
 */
uint8_t veda_tasks_create(void);

/**
 * @brief 하행 바이트를 프레임으로 조립해 검증된 위험 이벤트만 ctrl_task로 넘긴다.
 * @details 시작하면서 USART6 수신을 연다. 스케줄러가 뜬 뒤에 여는 이유는 미리 열면 큐가
 * 아직 없는 상태에서 바이트가 도착하기 때문이다. 이후 재무장은 각 콜백이 담당한다.
 *
 * DOWNLINK_RESYNC_MSEC 동안 바이트가 하나도 없고 조립 중이던 것이 있으면 조각을 버린다.
 */
void StartRxTask(void *argument);

/**
 * @brief 위험 이벤트를 접수해 채널의 목표 등급(desired)만 적는다.
 * @details 회선에 언제 어떤 순서로 나갈지는 sched_task 가 정한다. 여기서 송신도 ACK 대기도
 * 하지 않으므로 이 태스크는 블로킹되지 않는다 -- 4채널 이벤트가 한꺼번에 몰려도 cmd_queue 가
 * 밀리지 않는다는 뜻이고, 그것이 접수와 실행을 가른 이유다.
 *
 * ### 채널 범위 검사
 * base보다 작은 channel_id 는 32비트 뺄셈에서 감겨 아주 큰 값이 되므로, 한 번의 범위 검사가
 * '너무 작다'와 '너무 크다'를 함께 잡는다. base를 0으로 둔 빌드에서 "< base" 비교가 항상
 * 거짓이라 경고가 뜨는 것도 이 형태면 피할 수 있다. 거부한 값은 stat_last_bad_channel 에
 * 남겨 두어, 로그만 보고 '배선되지 않은 채널'과 'RPI_CHANNEL_ID_BASE 오설정'을 가른다.
 *
 * ### 규약 밖의 등급은 세되 버리지 않는다
 * 0/1/2 밖의 값(3, 10, 255 ...)은 회선이 깨졌거나 저쪽 규약이 우리보다 앞선 것이다.
 * 세어서 로그에 남기되, **이벤트를 버리지는 않는다.**
 *
 * 한때 여기서 continue 로 버렸는데 그게 위험했다. 버리면 Master 가 Slave 에게 아무것도
 * 보내지 않아 경광등이 직전 상태 그대로 굳는다 -- 잘못된 값 하나 때문에 이미 울리고 있던
 * 경보를 끌 수도, 꺼야 할 것을 끌 수도 없게 된다. risk_to_status() 가 모르는 값을 안전한
 * 쪽(NONE = 소등)으로 떨어뜨리고 그 결과를 그대로 전송하는 편이 낫다.
 *
 * 이것이 요구사항의 "잘못된 값을 임의로 변환하지 말 것"과 어긋나지 않는 이유: 그 규칙은
 * 회선 위 위험도를 해석하는 Slave 쪽 규칙이고, Slave 는 지금도 0/1/2 밖의 값을
 * VERDICT_BAD_RISK 로 거절한다. 여기는 RPi 가 보낸 상위 값을 우리 규약으로 정규화하는
 * 자리이고, 정규화 결과(항상 0/1/2)만 회선에 나간다.
 *
 * ### 지연 계측의 시작점은 '값이 바뀐 순간'뿐이다
 * 세 갈래로 나뉜다.
 *  1. `desired` 와 같은 값이 다시 왔다 -> stat_cmd_coalesced. 회선에 아무것도 나가지 않는다.
 *     같은 등급의 반복 명령까지 재면 스케줄러가 흡수해 회선에 안 나가는 것을 지연으로
 *     잘못 세게 된다.
 *  2. 값이 바뀌었지만 이미 `applied` 가 그 상태다(WARNING 으로 갔다가 서비스되기 전에
 *     NONE 으로 되돌아온 경우) -> 회선에 내보낼 것이 없으므로 '지연'이 아니다. 계측을
 *     취소한다. 여기서 걸어 두면 다음 주기 리프레시(2초)에야 풀려서 lat_max 가 2000ms 로
 *     잘못 찍힌다.
 *  3. 진짜로 바뀌었다 -> desired_tick 을 찍고 lat_pending 을 세운다.
 *
 * 같은 등급이 몇 번 오든 desired 한 칸을 덮어쓸 뿐이라 회선에는 아무것도 나가지 않는다.
 * 예전에는 여기서 곧바로 재전송했는데(RPi의 checkChannelMismatch 복구를 그대로 흘려
 * 보내려는 의도였다), 그 때문에 DANGER 연발이 전부 버스에 쌓여 뒤늦게 몰아서 재생됐다.
 * 복구 책임은 이제 sched_task 가 진다 -- ACK 재시도와 SCHED_REFRESH_MSEC 주기 재전송이
 * 그 자리를 대신한다. **둘 중 하나라도 빠지면 이 최적화는 위험하다.**
 */
void StartCtrlTask(void *argument);

/**
 * @brief 상행 큐를 비우는 유일한 USART6 송신자.
 */
void StartTxTask(void *argument);

/**
 * @brief HEARTBEAT_INTERVAL_MSEC마다 채널마다 한 장씩 HEARTBEAT를 올린다.
 * @details RPi는 채널별로 마지막 HEARTBEAT 시각을 따로 들고 있으므로(lastHeartbeatAt_),
 * 한 장으로 전체를 대표할 수 없다. 채널 수만큼 보내야 한다.
 *
 * osDelay가 아니라 osDelayUntil을 쓴다. osDelay는 '깨어난 뒤부터' 다시 세기 때문에
 * 루프 본문 시간이 매 주기 누적되고, 주기가 조금씩 늘어나 결국 dead 판정에 걸린다.
 */
void StartHeartbeatTask(void *argument);

#endif /* VEDA_TASKS_H */
