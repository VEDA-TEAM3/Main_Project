/**
 * @file test_sched_select.c
 * @brief sched_select.inc 의 채널 선택 규칙을 호스트에서 검사한다.
 *
 * 빌드/실행:
 *   gcc -Wall -Wextra -I../Core/Inc test_sched_select.c -o test_sched_select && ./test_sched_select
 *
 * 펌웨어를 굽지 않고 확인하려는 것은 세 가지다.
 *   1) DANGER 가 항상 먼저 나가는가
 *   2) 커서가 전진해서 굶는 채널이 없는가
 *   3) 주기 리프레시가 DANGER 전환을 밀어내지 않는가
 * 이 셋이 깨지면 실기에서는 "가끔 한 채널만 반응이 늦다"로만 보여서 원인을 찾기가 매우 어렵다.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* --- main.c 에서 가져온 최소한의 문맥 ------------------------------------- */
#define CHANNEL_COUNT 4U
#define SCHED_REFRESH_MSEC 2000U
#define SCHED_RETRY_BACKOFF_MSEC 100U
#define SCHED_NO_CHANNEL 0xFFU

#define VEDA_RISK_NONE 0
#define VEDA_RISK_WARNING 1
#define VEDA_RISK_DANGER 2

typedef struct
{
  volatile uint8_t desired_risk;
  uint8_t          applied_risk;
  uint32_t         last_tx_tick;
  uint16_t         retry;
} channel_ctrl_t;

static channel_ctrl_t channel_ctrl[CHANNEL_COUNT];
static uint8_t sched_cursor;

/** 테스트가 시각을 직접 정한다. */
static uint32_t fake_now;
static uint32_t osKernelGetTickCount(void) { return fake_now; }

#include "sched_select.inc"

/* --- 검사 ----------------------------------------------------------------- */

/** 모든 채널을 "맞춰졌고 방금 보냈다"로 되돌린다. */
static void reset_all(void)
{
  uint8_t i;

  fake_now = 100000U;   /* 틱이 충분히 흐른 뒤에서 시작한다 */
  sched_cursor = 0U;

  for (i = 0U; i < CHANNEL_COUNT; ++i)
  {
    channel_ctrl[i].desired_risk = (uint8_t)VEDA_RISK_NONE;
    channel_ctrl[i].applied_risk = (uint8_t)VEDA_RISK_NONE;
    channel_ctrl[i].last_tx_tick = fake_now;
    channel_ctrl[i].retry = 0U;
  }
}

/** sched_task 의 두 패스를 그대로 흉내낸다. */
static uint8_t select_one(void)
{
  uint8_t ch = pick_pending(1U);
  if (ch == SCHED_NO_CHANNEL)
  {
    ch = pick_pending(0U);
  }
  return ch;
}

/** 고른 채널을 "보냈다"로 처리하고 커서를 전진시킨다(sched_task 와 같은 순서). */
static void commit(uint8_t ch)
{
  channel_ctrl[ch].applied_risk = channel_ctrl[ch].desired_risk;
  channel_ctrl[ch].last_tx_tick = fake_now;
  sched_cursor = (uint8_t)((ch + 1U) % CHANNEL_COUNT);
}

static void test_idle_selects_nothing(void)
{
  reset_all();
  assert(select_one() == SCHED_NO_CHANNEL);
}

static void test_danger_beats_warning(void)
{
  reset_all();
  /* 커서 바로 앞에 WARNING 을, 뒤쪽에 DANGER 를 둔다. 순서만 보면 CH0 이 먼저다. */
  channel_ctrl[0].desired_risk = (uint8_t)VEDA_RISK_WARNING;
  channel_ctrl[2].desired_risk = (uint8_t)VEDA_RISK_DANGER;

  assert(select_one() == 2U);   /* 우선순위가 순서를 이긴다 */
}

static void test_round_robin_starts_at_cursor(void)
{
  reset_all();
  channel_ctrl[0].desired_risk = (uint8_t)VEDA_RISK_WARNING;
  channel_ctrl[3].desired_risk = (uint8_t)VEDA_RISK_WARNING;
  sched_cursor = 2U;

  assert(select_one() == 3U);   /* 0이 아니라 커서부터 훑는다 */
}

/**
 * 같은 등급의 채널이 여럿일 때 하나가 굶지 않는지 본다.
 * 커서가 전진하지 않으면 이 검사가 무한히 CH1 만 고른다.
 */
static void test_no_starvation(void)
{
  uint8_t seen[CHANNEL_COUNT] = {0};
  uint8_t i;

  reset_all();
  for (i = 0U; i < CHANNEL_COUNT; ++i)
  {
    channel_ctrl[i].desired_risk = (uint8_t)VEDA_RISK_DANGER;
  }

  for (i = 0U; i < CHANNEL_COUNT; ++i)
  {
    const uint8_t ch = select_one();
    assert(ch != SCHED_NO_CHANNEL);
    assert(seen[ch] == 0U);     /* 같은 채널을 두 번 고르면 굶는 채널이 생긴 것이다 */
    seen[ch] = 1U;
    commit(ch);
  }

  assert(select_one() == SCHED_NO_CHANNEL);   /* 한 바퀴에 전부 처리됐다 */
}

static void test_refresh_fires_when_stale(void)
{
  reset_all();
  assert(select_one() == SCHED_NO_CHANNEL);

  fake_now += SCHED_REFRESH_MSEC - 1U;
  assert(select_one() == SCHED_NO_CHANNEL);   /* 아직 이르다 */

  fake_now += 1U;
  assert(select_one() == 0U);                 /* 값은 그대로지만 다시 보낸다 */
}

/**
 * 리프레시가 DANGER 전환을 밀어내면 안 된다. 이게 깨지면 경보가 20ms씩 늦어지는데,
 * 채널이 늘수록 누적돼서 실기에서 "가끔 반응이 늦다"로 나타난다.
 */
static void test_refresh_never_preempts_danger(void)
{
  reset_all();
  fake_now += SCHED_REFRESH_MSEC;             /* 전 채널이 리프레시 대상이 됐다 */
  channel_ctrl[3].desired_risk = (uint8_t)VEDA_RISK_DANGER;

  assert(select_one() == 3U);
}

/** 고른 채널이 실패했다고 처리한다(applied 는 그대로, retry 증가). */
static void fail(uint8_t ch)
{
  channel_ctrl[ch].last_tx_tick = fake_now;
  channel_ctrl[ch].retry++;
  sched_cursor = (uint8_t)((ch + 1U) % CHANNEL_COUNT);
}

/**
 * 응답하지 않는 DANGER 채널이 나머지를 굶기면 안 된다.
 *
 * 실기에서 이렇게 나타났다: CH3(index 2)가 DANGER인데 ACK가 오지 않아 계속 재시도되고,
 * 전송 순서가 3->0->2->1 로 CH3만 순서를 앞질렀다. 백오프가 없으면 우선순위 패스가
 * 대기 중인 DANGER를 매 틱 돌려주므로 2차 패스가 아예 실행되지 않고, 살아 있는 나머지
 * 채널의 주기 리프레시가 통째로 멈춘다.
 */
static void test_failing_danger_does_not_starve_others(void)
{
  uint8_t i;
  uint8_t served_others = 0U;

  reset_all();
  channel_ctrl[2].desired_risk = (uint8_t)VEDA_RISK_DANGER;   /* 영영 ACK가 오지 않는 채널 */

  /* 첫 시도는 대기 없이 즉시 나가야 한다. */
  assert(select_one() == 2U);
  fail(2U);

  /* 백오프 동안에는 다른 채널이 서비스될 수 있어야 한다. 전 채널을 리프레시 대상으로
   * 만들어 두고, 20ms 틱을 백오프 구간만큼 돌려 본다. */
  for (i = 0U; i < CHANNEL_COUNT; ++i)
  {
    channel_ctrl[i].last_tx_tick = fake_now - SCHED_REFRESH_MSEC;
  }
  channel_ctrl[2].last_tx_tick = fake_now;   /* 방금 실패했다 */

  /* CH3를 뺀 세 채널이 각각 한 번씩 나와야 한다. 백오프가 없으면 첫 회차부터 CH3가
   * 다시 나와 이 루프가 터진다. */
  for (i = 0U; i < (CHANNEL_COUNT - 1U); ++i)
  {
    uint8_t ch;

    fake_now += 20U;            /* 한 틱. 세 번 돌아도 백오프(100ms) 안이다. */
    ch = select_one();
    assert(ch != SCHED_NO_CHANNEL);
    assert(ch != 2U);           /* 백오프 중이므로 CH3가 또 나오면 안 된다 */
    served_others = 1U;
    commit(ch);
  }
  assert(served_others == 1U);

  /* 나머지가 다 맞춰졌고 CH3는 아직 백오프 중이다 -- 이 틱은 쉬어야 한다. */
  assert(select_one() == SCHED_NO_CHANNEL);

  /* 백오프가 지나면 다시 잡혀야 한다 -- 양보시키는 것이지 포기하는 것이 아니다. */
  fake_now += SCHED_RETRY_BACKOFF_MSEC;
  assert(select_one() == 2U);
}

/** 틱 접힘. 부호 없는 뺄셈을 쓰지 않으면 여기서 엉뚱하게 리프레시가 터진다. */
static void test_tick_wraparound(void)
{
  uint8_t i;

  reset_all();

  /* 틱이 감긴 직후로 옮긴다. 채널을 하나만 옮기면 나머지가 '아주 오래된' 것으로 보여
   * 검사가 엉뚱한 채널을 집는다 -- 전부 같이 옮겨야 한다. */
  fake_now = 10U;
  for (i = 0U; i < CHANNEL_COUNT; ++i)
  {
    channel_ctrl[i].last_tx_tick = 0xFFFFFF00U;   /* 감기 직전에 보냈다 -> 경과 272ms */
  }
  assert(select_one() == SCHED_NO_CHANNEL);

  channel_ctrl[0].last_tx_tick = 0xFFFF0000U;     /* 경과 65546ms */
  assert(select_one() == 0U);
}

int main(void)
{
  test_idle_selects_nothing();
  test_danger_beats_warning();
  test_round_robin_starts_at_cursor();
  test_no_starvation();
  test_refresh_fires_when_stale();
  test_refresh_never_preempts_danger();
  test_failing_danger_does_not_starve_others();
  test_tick_wraparound();

  printf("sched_select: all checks passed\n");
  return 0;
}
