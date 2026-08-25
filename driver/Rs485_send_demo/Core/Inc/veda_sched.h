/**
 * @file veda_sched.h
 * @brief 스케줄러 태스크 -- **이 시스템의 유일한 RS-485 송신자.**
 *
 * ### 접수와 실행의 분리
 * ctrl_task 는 검증된 이벤트를 받아 `desired`(원하는 등급)만 적고 끝낸다. 회선에 언제 어떤
 * 순서로 나갈지는 전부 여기가 정한다. 이 분리가 스케줄러의 전부이고, 그 이유는
 * veda_types.h 의 channel_ctrl_t 설명에 있다.
 *
 * ### 한 틱에 한 채널
 * SCHED_TICK_MSEC(20ms)마다 `desired != applied` 인 채널을 **하나만** 골라 내보내고 ACK를
 * 기다린다. 버스 점유가 결정적이 되고, Slave의 단일 ACK 슬롯(ack_pending)이 다음 명령에
 * 덮어써지지 않을 만큼의 간격도 여기서 보장된다.
 *
 * ### 우선순위: DANGER 먼저
 * 경보를 켜는 일이 끄는 일보다 항상 급하다. 두 패스(1차 DANGER 전용, 2차 라운드로빈)가
 * 같은 커서를 공유하므로 DANGER 채널이 여럿이어도 자기들끼리 라운드로빈이 돌고, DANGER가
 * 계속 들어와도 NONE/WARNING이 밀리는 것은 한 틱(20ms)뿐이라 눈에 보이지 않는다.
 *
 * ### 재시도는 따로 만들지 않았다
 * ACK 를 받아야만 `applied` 를 갱신한다. 실패하면 `applied` 가 그대로 남아 다음 틱에
 * `desired != applied` 로 같은 채널이 자연히 다시 잡힌다 -- 재시도 큐도 타이머도 필요 없다.
 *
 * ACK 타임아웃(100ms)이 틱(20ms)보다 길다. 타임아웃이 나면 osDelayUntil 의 목표 시각이 이미
 * 지나 있어 즉시 반환하고 백투백으로 다음 채널을 잡는다. 최악(4채널 전부 무응답)이라도
 * 커서가 매 회차 전진하므로 굶는 채널은 없다.
 *
 * ### 로직은 네 개의 .inc 에 있다 (추상화가 아니라 검증)
 * | 파일 | 내용 | 호스트 테스트 |
 * |---|---|---|
 * | `sched_select.inc`   | `pick_pending()` — 이번 틱에 누구를 고르나 | `tools/test_sched_select.c` |
 * | `sched_dispatch.inc` | `dispatch_channel()` / `sched_tick()` — 고른 뒤 상태 전이 | `tools/test_sched_loop.c` |
 * | `ack_match.inc`      | 늦은·남의 ACK 대조 + 큐 비우기 | `tools/test_ack_match.c` |
 *
 * 호스트 테스트가 **같은 소스를 그대로 포함**해 펌웨어와 갈라지지 않게 한다.
 * 스케줄러를 고쳤으면 커밋 전에 tools/ 의 테스트를 전부 돌리고 mutation_check 도 볼 것.
 *
 * ### 부팅 소등을 여기서 직접 쏘지 않는다
 * applied_risk 가 SCHED_RISK_UNKNOWN 이라 desired(부팅값 NONE)와 어긋나 있으므로, 루프의
 * 첫 몇 틱에서 평소의 경로로 나간다. 예전에는 ACK를 보지 않고 쏜 뒤 applied 를 NONE 으로
 * 적었는데, 그러면
 *   - Slave 가 아직 부팅 중(LD2 점멸 ~1초)이라 그 프레임을 못 받아도 우리는 확인된 줄 알고,
 *   - 상행은 부팅 순간부터 '초록'을 싣고 올라가 RPi/Qt 에 전이가 한 번도 생기지 않았다.
 * 스케줄러에 맡기면 ACK 로 확인될 때까지 재시도하고, 확인된 그 순간 '미확인 -> 초록'
 * 전이가 상행 ACK 로 한 장 올라간다. 그것이 Qt 가 부팅 상태를 알게 되는 유일한 신호다.
 */

#ifndef VEDA_SCHED_H
#define VEDA_SCHED_H

/**
 * @brief SCHED_TICK_MSEC마다 채널 하나씩 RS-485로 내보낸다. huart1 송신자는 이 태스크뿐이다.
 * @details 루프가 책임지는 것은 **20ms 주기(osDelayUntil)와 틱 계수뿐**이다. 선택과 송신은
 * sched_dispatch.inc 의 sched_tick() 에 있다 -- 여기서 풀어 쓰면 호스트 테스트와 실기가
 * 갈라진다.
 *
 * stat_sched_ticks 를 sched_tick() 안이 아니라 루프에서 세는 이유: '루프를 한 바퀴 돈 것'을
 * 세야 osDelayUntil 의 실제 주기가 드러난다. .inc 를 건드리지 않으므로 호스트 테스트에도
 * 영향이 없다.
 *
 * 시작하면서 rs485_de_init() 과 ACK 수신 개방(veda_rs485_rx_arm)을 한다. 그래서 이 태스크의
 * 생성 실패는 **조용한 고장**이 된다 -- 시스템은 멀쩡히 부팅하고 hb_task 가 부팅값을 계속
 * 올려 보내 RPi 는 Master 가 건강하다고 판단하는데, 경보가 영원히 울리지 않는다.
 * main.c 가 여섯 핸들을 모두 검사하는 이유가 이것이다.
 */
void StartSchedTask(void *argument);

#endif /* VEDA_SCHED_H */
