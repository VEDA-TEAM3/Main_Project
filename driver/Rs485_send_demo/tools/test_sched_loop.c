/**
 * @file test_sched_loop.c
 * @brief 스케줄러의 "닫힌 루프"를 호스트에서 통째로 돌려 상태가 제대로 갱신되는지 검사한다.
 *
 * 빌드/실행 (tools/ 에서):
 *   gcc -Wall -Wextra -I../Core/Inc test_sched_loop.c -o test_sched_loop && ./test_sched_loop
 *
 * test_sched_select.c 와 무엇이 다른가:
 *   test_sched_select.c 는 pick_pending() 하나(누구를 고르는가)만 본다. 고른 뒤의 상태 전이
 *   (applied_risk/last_tx_tick/retry)는 그 파일의 commit()/fail() 이 '손으로 흉내낸 값'이라,
 *   실제 dispatch_channel() 이 다르게 갱신하면 테스트는 통과하는데 실기는 틀린다(TESTING.md
 *   의 "알려진 구멍"). 이 파일은 sched_dispatch.inc 를 그대로 포함해 실제 dispatch_channel()/
 *   sched_tick() 을 돌린다 -- 그 구멍을 메운다.
 *
 * 무엇을 재현하나:
 *   RPi(카메라 30fps) 가 33ms 마다 랜덤 위험 이벤트를 던지고, 스케줄러는 20ms 마다 한 채널씩
 *   내보낸다. 두 개의 가상 클럭으로 이 흐름을 실제 대기 없이 재현하고, 랜덤 주입이 멎으면
 *   모든 채널이 명령대로 수렴하는지(= 제대로 update 되는지) 자동으로 확인한다.
 *
 * 왜 랜덤인가:
 *   손으로 짠 시나리오는 짠 사람이 상상한 순서만 본다. 33ms 랜덤 주입은 우선순위 역전,
 *   경합, 코얼레싱이 어떤 순서로 엮여도 수렴이 깨지지 않는지를 넓게 훑는다.
 *   대신 반드시 재현 가능해야 한다 -- 시드를 고정하고, 실패하면 그 시드를 찍는다.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- main.c 에서 가져온 최소한의 문맥 (값은 main.c 와 반드시 같아야 한다) ------------- */
#define CHANNEL_COUNT 4U
#define CHANNELS_PER_SLAVE 2U
#define SCHED_TICK_MSEC 20U
#define SCHED_REFRESH_MSEC 2000U
#define SCHED_RETRY_BACKOFF_MSEC 500U
#define SCHED_NO_CHANNEL 0xFFU
#define SCHED_RISK_UNKNOWN 0xFFU   /* applied_risk 의 부팅값 = "아직 확인 안 됨" */
#define RS485_ACK_REQUIRED 1
#define RS485_ACK_TIMEOUT_MSEC 100U
#define VEDA_UPLINK_REASON_ACK 0

/**
 * 회선에서 실제로 흘러가는 시간. 지연을 재려면 이걸 모델링해야 한다 -- 가짜 ACK가
 * 즉답하면 실패한 채널이 공짜가 되어, 실기에서 가장 크게 밀리는 구간이 사라진다.
 *   TX  : "S1:CH1:R2\n" 10바이트 @115200 8N1 ≈ 0.9ms
 *   ACK : Slave 태스크가 10ms 주기로 폴링해 되보낸다 -> 최악 한 주기
 */
#define TX_TIME_MSEC 1U

/**
 * Slave 루프 한 바퀴. ACK 는 ISR 이 아니라 이 루프가 보내므로, 명령이 도착하고 ACK 가
 * 회선에 실리기까지 최악 한 바퀴가 걸린다. Rs485_receive_demo 의 StartDefaultTask 기준:
 *   osDelay(10)                     = 10ms
 *   slave_print (RX ... 판정, ~47자 @115200, 블로킹) ≈ 4ms
 *   neopixel_service (DMA 송신 대기) ≈ 2ms
 * 합쳐서 약 16ms. Master 의 틱(20ms)과 이 값의 차이가 곧 단일 ACK 슬롯의 여유다.
 */
#define SLAVE_LOOP_MSEC 16U

#define VEDA_RISK_NONE 0
#define VEDA_RISK_WARNING 1
#define VEDA_RISK_DANGER 2

/* RPi 명령 주기(카메라 30fps ~= 33ms) 와 소크 길이 */
#define INJECT_INTERVAL_MSEC 33U
#define SOAK_MSEC 30000U      /* 한 시드당 랜덤 주입 시간 */
#define QUIET_MSEC 1000U      /* 주입을 멈추고 수렴을 확인하기까지 기다리는(가상) 시간 */
#define SEED_COUNT 300U       /* 서로 다른 랜덤 순서를 몇 개 훑을지 */

typedef struct
{
  volatile uint8_t desired_risk;
  uint8_t          applied_risk;
  uint32_t         last_tx_tick;
  uint16_t         retry;
  /* 반응 지연 계측 (main.c 와 같은 필드) */
  uint32_t         desired_tick;
  uint8_t          lat_pending;
  uint8_t          lat_risk;
} channel_ctrl_t;

typedef struct
{
  uint8_t risk_level;
  uint8_t siren_on;
  uint8_t buzzer_on;
  uint8_t led_red;
  uint8_t led_yellow;
  uint8_t led_green;
} channel_status_t;

/* --- 스케줄러가 기대하는 전역 -------------------------------------------------------- */
static channel_ctrl_t   channel_ctrl[CHANNEL_COUNT];
static channel_status_t channel_status[CHANNEL_COUNT];
static uint8_t          sched_cursor;

static uint32_t stat_rs485_tx_fail;
static uint32_t stat_sched_retry;
static uint32_t stat_sched_refresh;
static uint32_t stat_lat_max;
static uint32_t stat_lat_danger_max;

/** 회선에 실제로 나간 프레임 수. 버스 점유 상한 검사에 쓴다. */
static uint32_t tx_count;
/** 채널별 송신 횟수. 커서가 전진하는지(=공정한가) 보는 데 쓴다. */
static uint32_t tx_per_channel[CHANNEL_COUNT];

/* --- 가상 시각 ---------------------------------------------------------------------- */
static uint32_t fake_now;
static uint32_t osKernelGetTickCount(void) { return fake_now; }

/* --- 뮤텍스/큐 자리끼우기 (테스트에는 경합이 없다) --------------------------------- */
typedef int osMutexId_t;
static osMutexId_t channel_status_mutex = 0;
#define osWaitForever 0xFFFFFFFFu
static int  osMutexAcquire(osMutexId_t m, uint32_t t) { (void)m; (void)t; return 0; }
static void osMutexRelease(osMutexId_t m) { (void)m; }
static void drain_ack_queue(void) { /* 실기에서만 의미가 있다 */ }
static uint32_t uplink_count;
static void enqueue_uplink(uint8_t ch, uint8_t reason) { (void)ch; (void)reason; ++uplink_count; }

/* --- 가짜 Slave 모델 (이 여섯 개 seam 만 실기와 다르다) ---------------------------- */
/**
 * 채널마다 독립적으로 모델링한다. 물리 보드 한 대(=CHANNELS_PER_SLAVE 채널)를 죽이려면
 * set_board_alive() 로 그 보드의 채널을 함께 끈다.
 *   reachable = 전원 켜짐 + MY_SLAVE_ID 일치 (프레임을 받아 GPIO 를 실제로 구동한다)
 *   will_ack  = ACK 가 회선을 타고 Master 까지 돌아온다 (양방향 배선이 정상)
 *   applied   = 그 채널이 지금 물리적으로 표시하고 있는 등급
 */
typedef struct
{
  int     reachable;
  int     will_ack;
  uint8_t applied;
} slave_model_t;

static slave_model_t slave[CHANNEL_COUNT];

/**
 * 보드(=Slave 한 대) 하나의 ACK 슬롯. Rs485_receive_demo 의 ack_pending / ack_channel /
 * ack_risk 를 그대로 옮긴 것이다.
 *
 * !! 슬롯이 하나뿐이고 보드 단위다(채널 단위가 아니다). 실제 코드가 이렇다:
 *
 *      if (ack_pending == 0U) { ack_channel = ...; ack_risk = ...; ack_pending = 1U; }
 *
 *    즉 앞 ACK 가 아직 안 나갔는데 새 명령이 오면, **명령은 적용되지만 그 ACK 는 조용히
 *    사라진다.** Master 는 100ms 타임아웃을 먹고 재전송한다. 한 보드가 2채널을 담당하므로
 *    CH1 과 CH2 로 연달아 보내는 경우가 정확히 이 조건이다.
 *
 * 이 모델이 없으면 "ACK 는 언제나 돌아온다"가 되어, 실기에서 유일하게 ACK 가 조용히
 * 사라지는 경로를 시뮬레이션이 통째로 놓친다.
 */
typedef struct
{
  int      armed;        /**< ACK 가 예약돼 있다 (ack_pending) */
  uint8_t  channel;      /**< 예약된 ACK 의 채널 (1-based) */
  uint8_t  risk;         /**< 예약된 ACK 의 등급 */
  uint32_t ready_at;     /**< 그 ACK 가 회선에 실리는 시각 */
} board_model_t;

#define BOARD_COUNT (CHANNEL_COUNT / CHANNELS_PER_SLAVE)
static board_model_t board[BOARD_COUNT];

/** 단일 슬롯이 덮여 ACK 가 사라진 횟수. 0 이 아니면 틱이 Slave 루프보다 빠른 것이다. */
static uint32_t stat_ack_slot_lost;

/**
 * 살아 있는 채널이 명령 없이 방치돼도 되는 최대 시간.
 *
 * Slave 에는 링크 두절 failsafe 가 없다(fail-loud). 그래서 어긋난 Slave 를 되돌리는 경로는
 * Master 의 주기 리프레시 하나뿐이고, 그것이 실제로 도는지가 곧 복구 능력이다.
 * !! 이 값을 SCHED_REFRESH_MSEC 에서 파생시키면 안 된다. 한때 `SCHED_REFRESH_MSEC * 2` 로
 *    두었는데, 그러면 리프레시를 약화시키는 변경(2000 -> 20000)이 상한까지 함께 늘려서
 *    검사가 조용히 통과한다. 실제로 뮤테이션 검사에서 이 구멍이 드러났다.
 *    검사 대상과 합격선은 서로 독립이어야 한다 -- 그래서 절대값으로 못박는다.
 *
 * 4000ms 근거: 정상 리프레시 주기 2000ms + 재시도가 끼어들어 한 주기쯤 밀리는 여유.
 * 측정값은 2128ms 다.
 */
#define REFRESH_GAP_BOUND_MSEC 4000U

/** 채널별로 Slave 가 실제로 명령을 적용한 마지막 시각과 그 최대 공백. */
static uint32_t slave_last_apply[CHANNEL_COUNT];
static uint32_t slave_max_gap[CHANNEL_COUNT];

/**
 * ACK 를 기다리는 '도중'에 ctrl_task 가 desired 를 바꾸는 상황을 재현할지 여부.
 *
 * 왜 따로 필요한가: run() 은 주입과 틱을 번갈아 처리하므로 inject_random() 이 언제나
 * sched_tick() '사이'에서만 돈다. 그런데 실기에서 ctrl_task 와 sched_task 는 같은 우선순위의
 * 별개 태스크이고 ACK 대기가 최대 100ms 라, RPi 명령이 그보다 촘촘하면
 * **dispatch_channel() 이 ACK 를 기다리는 동안 desired 가 바뀌는 일이 일상적으로 일어난다.**
 * 그 인터리빙이 지금까지 한 번도 검사되지 않았다.
 *
 * 이것이 노리는 고장은 하나다: 늦게 도착한(또는 다른 등급의) ACK 가 그 사이 바뀐 새 상태를
 * 적용된 것으로 기록하는 것 -- 이 시스템에서 가장 위험한 거짓말이다.
 * 아래 last_sent_risk[] 불변식이 그것을 직접 잡는다.
 */
static int interleave_enabled;

/**
 * 채널별로 가장 최근에 회선에 실제로 나간 등급. dispatch_channel() 이 스냅샷한 want 다.
 *
 * 불변식: applied_risk 가 바뀌었다면 그 새 값은 **반드시 이 값과 같아야 한다.**
 * 옛 ACK 가 현재 상태를 갱신하면 여기서 즉시 걸린다 -- desired 가 그 사이 무엇으로 바뀌었든
 * 상관없이 성립하는 검사라, 인터리빙이 있든 없든 그대로 쓸 수 있다.
 */
static uint8_t last_sent_risk[CHANNEL_COUNT];

/** send: 회선에 바이트를 밀어넣는 데는 언제나 성공한다(HAL 관점). Slave 가 살아 있으면
 *  그 프레임을 받아 실제로 적용한다. 죽어 있으면 아무 일도 안 일어난다 -- 실패는 ACK 없음으로만
 *  드러난다. 실기의 send_channel_command() 주석과 같은 의미다. */
static uint8_t send_channel_command(uint8_t channel_index, uint8_t risk_level)
{
  const uint8_t b = (uint8_t)(channel_index / CHANNELS_PER_SLAVE);

  ++tx_count;
  ++tx_per_channel[channel_index];
  last_sent_risk[channel_index] = risk_level;   /* applied 전이 불변식의 기준값 */
  fake_now += TX_TIME_MSEC;          /* 프레임이 회선을 지나가는 시간 */

  if (!slave[channel_index].reachable)
  {
    return 1U;   /* 회선에는 나갔다. 보드가 없으니 아무 일도 일어나지 않는다. */
  }

  /* ISR 이 GPIO 를 먼저 쓴다 -- 적용은 ACK 슬롯 상태와 무관하다.
   * Slave 의 failsafe 타이머도 이 순간 초기화된다(VERDICT_APPLIED). */
  slave[channel_index].applied = risk_level;
  {
    const uint32_t gap = fake_now - slave_last_apply[channel_index];
    if (gap > slave_max_gap[channel_index])
    {
      slave_max_gap[channel_index] = gap;
    }
    slave_last_apply[channel_index] = fake_now;
  }

  /* 예약된 ACK 가 이미 나갈 시각을 지났으면 슬롯은 비어 있다. */
  if (board[b].armed && board[b].ready_at <= fake_now)
  {
    board[b].armed = 0;
  }

  if (board[b].armed)
  {
    /* 앞 ACK 가 아직 안 나갔다 -- 이번 ACK 는 조용히 사라진다(실제 Slave 동작). */
    ++stat_ack_slot_lost;
  }
  else
  {
    board[b].armed = 1;
    board[b].channel = (uint8_t)(channel_index + 1U);
    board[b].risk = risk_level;
    board[b].ready_at = fake_now + SLAVE_LOOP_MSEC;
  }
  return 1U;
}

/**
 * ack: Slave 가 살아 있고 ACK 가 돌아오며 방금 적용한 값이 보낸 값과 같을 때만 성공.
 * 성공이면 Slave 의 폴링 지연만큼, 실패면 Master 의 타임아웃만큼 시간이 실제로 흐른다.
 * 이 시간이 곧 실패한 채널이 버스를 붙잡는 시간이라, 지연 상한 검사의 핵심 입력이다.
 */
/** 아래에 정의. 인터리빙 주입이 ctrl_task 와 정확히 같은 경로를 타게 하려고 앞당겨 선언한다. */
static void set_desired(uint8_t ch, uint8_t risk);

static uint8_t wait_for_slave_ack(uint8_t channel_index, uint8_t risk_level)
{
  const uint8_t b = (uint8_t)(channel_index / CHANNELS_PER_SLAVE);
  const uint8_t want_channel = (uint8_t)(channel_index + 1U);

  /* ACK 대기 '도중'의 desired 변경(실기의 ctrl_task 선점). 이 자리인 이유:
   * dispatch_channel() 은 want 를 이미 스냅샷했고 프레임도 이미 회선에 나갔다. 즉 지금부터
   * ACK 가 돌아올 때까지가 정확히 문제의 창이다.
   * 절반은 지금 대기 중인 바로 그 채널을, 절반은 아무 채널을 건드려 둘 다 훑는다 --
   * 같은 채널을 바꾸는 쪽이 "보낸 값과 desired 가 어긋난 채 ACK 가 온다"는 어려운 경우다. */
  if (interleave_enabled && (rand() % 3) == 0)
  {
    const uint8_t ch = (rand() % 2) ? channel_index
                                    : (uint8_t)(rand() % CHANNEL_COUNT);
    set_desired(ch, (uint8_t)(rand() % 3));
  }

  if (slave[channel_index].reachable && slave[channel_index].will_ack &&
      board[b].armed &&
      board[b].channel == want_channel && board[b].risk == risk_level &&
      board[b].ready_at <= (fake_now + RS485_ACK_TIMEOUT_MSEC))
  {
    fake_now = board[b].ready_at;   /* Slave 루프가 깨어나 ACK 를 실을 때까지 기다린다 */
    board[b].armed = 0;
    return 1U;
  }

  /* ACK 가 없거나(슬롯을 잃었거나 보드가 죽었거나) 내용이 다르다 -- 타임아웃을 먹는다. */
  fake_now += RS485_ACK_TIMEOUT_MSEC;
  if (board[b].armed && board[b].ready_at <= fake_now)
  {
    board[b].armed = 0;   /* 그 사이 나간 늦은 ACK 는 drain_ack_queue() 가 버린다 */
  }
  return 0U;
}

/** risk_to_status: 실기와 동치인 최소 구현. dispatch_channel 이 성공 시 channel_status 를
 *  채우는 데만 쓴다(테스트는 값 범위 정도만 확인한다). */
static void risk_to_status(uint8_t risk_level, channel_status_t *out)
{
  out->risk_level = risk_level;
  out->siren_on   = (uint8_t)(risk_level != VEDA_RISK_NONE);
  out->buzzer_on  = (uint8_t)(risk_level == VEDA_RISK_DANGER);
  out->led_red    = (uint8_t)(risk_level == VEDA_RISK_DANGER);
  out->led_yellow = (uint8_t)(risk_level == VEDA_RISK_WARNING);
  out->led_green  = (uint8_t)(risk_level == VEDA_RISK_NONE);
}

/* 실기와 같은 소스를 그대로 돌린다. select 가 먼저다(dispatch 가 pick_pending 을 쓴다). */
#include "sched_select.inc"
#include "sched_dispatch.inc"

/* --- 검사 도구 ---------------------------------------------------------------------- */
static unsigned g_seed;

/** 실패하면 시드와 시각을 찍고 멈춘다. 랜덤 테스트에서 이게 없으면 재현이 불가능하다. */
#define CHECK(cond, ...)                                                        \
  do {                                                                          \
    if (!(cond)) {                                                              \
      fprintf(stderr, "FAIL (seed=%u t=%ums): ", g_seed, fake_now);            \
      fprintf(stderr, __VA_ARGS__);                                             \
      fprintf(stderr, "\n");                                                    \
      abort();                                                                  \
    }                                                                           \
  } while (0)

/** 각 채널이 지금까지 한 번이라도 desired 였던 등급. 안전성 검사에 쓴다. */
static uint8_t ever_desired[CHANNEL_COUNT][3];

/** desired 를 바꾸는 유일한 통로. ever_desired 도 함께 갱신한다 -- 그래야 아래 안전성
 *  검사(applied 는 언제나 과거 desired 중 하나)가 정확해진다. */
static void set_desired(uint8_t ch, uint8_t risk)
{
  /* ctrl_task 와 같은 규칙: 값이 '바뀐' 순간만 지연 계측의 시작점이다.
   * 단, 이미 그 상태가 Slave 에 들어가 있으면 보낼 것이 없으므로 지연이 아니다. */
  if (risk != channel_ctrl[ch].desired_risk)
  {
    if (risk == channel_ctrl[ch].applied_risk)
    {
      channel_ctrl[ch].lat_pending = 0U;
    }
    else
    {
      channel_ctrl[ch].desired_tick = fake_now;
      channel_ctrl[ch].lat_risk = risk;
      channel_ctrl[ch].lat_pending = 1U;
    }
  }
  channel_ctrl[ch].desired_risk = risk;
  ever_desired[ch][risk] = 1U;
}

static void set_board_alive(uint8_t board, int alive)
{
  uint8_t c;
  for (c = 0U; c < CHANNELS_PER_SLAVE; ++c)
  {
    const uint8_t ch = (uint8_t)(board * CHANNELS_PER_SLAVE + c);
    slave[ch].reachable = alive;
    slave[ch].will_ack  = alive;
  }
}

/** 매 틱 확인하는 안전성: applied 는 항상 규약 안(0/1/2)이고, 과거에 desired 였던 값이어야 한다.
 *  임의값이 applied 에 들어오면(예: dispatch 가 want 아닌 값을 쓰면) 여기서 즉시 잡힌다. */
static void check_safety(void)
{
  uint8_t ch;
  for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
  {
    const uint8_t a = channel_ctrl[ch].applied_risk;
    /* 부팅값(미확인)은 아직 아무 것도 확인되지 않았다는 뜻이라 아래 두 검사의 대상이 아니다.
     * 이 값은 회선에 나가지도, 상행에 실리지도 않는다 -- 나가는 것은 언제나 desired 쪽이다. */
    if (a == SCHED_RISK_UNKNOWN) { continue; }
    CHECK(a <= 2U, "applied out of range: ch=%u applied=%u", ch, a);
    CHECK(ever_desired[ch][a], "applied value never desired: ch=%u applied=%u", ch, a);
  }
}

/** 랜덤 위험 이벤트 하나를 접수한다. ctrl_task 와 같이 규약 밖 값은 NONE 으로 정규화한다. */
static void inject_random(void)
{
  const uint8_t ch = (uint8_t)(rand() % CHANNEL_COUNT);
  const int roll = rand() % 20;    /* 약 5% 는 규약 밖 값 */
  uint8_t risk;

  if      (roll < 6)  risk = VEDA_RISK_NONE;
  else if (roll < 12) risk = VEDA_RISK_WARNING;
  else if (roll < 18) risk = VEDA_RISK_DANGER;
  else                risk = (uint8_t)(3 + (rand() % 250));   /* 3..252: 규약 밖 */

  /* ctrl_task 의 risk_to_status() 는 규약 밖 등급을 안전한 쪽(NONE)으로 떨어뜨린다. */
  if (risk > VEDA_RISK_DANGER) risk = VEDA_RISK_NONE;

  set_desired(ch, risk);
}

/**
 * 두 개의 가상 클럭(주입 33ms / 틱 20ms)을 이벤트 순서대로 돌린다. 실제 대기는 없다.
 * @param duration_ms 몇 ms(가상) 동안 돌릴지
 * @param inject 1이면 33ms마다 랜덤 이벤트를 주입한다
 */
static void run(uint32_t duration_ms, int inject)
{
  const uint32_t end = fake_now + duration_ms;
  uint32_t next_inject = fake_now + INJECT_INTERVAL_MSEC;
  uint32_t next_tick   = fake_now + SCHED_TICK_MSEC;

  while (fake_now < end)
  {
    if (inject && next_inject <= next_tick)
    {
      /* 주입이 먼저다. 시각을 되돌리지 않도록 이미 지난 주입은 지금 시각에 처리한다
       * (앞 회차의 ACK 대기가 주입 시점을 넘겨 버린 경우). */
      if (next_inject > fake_now)
      {
        fake_now = next_inject;
      }
      next_inject += INJECT_INTERVAL_MSEC;
      inject_random();
    }
    else
    {
      if (next_tick > fake_now)
      {
        fake_now = next_tick;
      }
      next_tick += SCHED_TICK_MSEC;

      /* sched_tick() 안에서 송신·ACK 대기로 시간이 흐른다.
       *
       * applied 전이 불변식을 그 앞뒤로 건다: 이번 틱에 applied 가 바뀌었다면 그 새 값은
       * **방금 회선에 나간 값**이어야 한다. 늦은 ACK 나 남의 ACK 가 현재 상태를 갱신하면
       * 여기서 즉시 걸린다 -- desired 가 그 사이 무엇으로 바뀌었든 상관없이 성립하므로
       * 인터리빙 주입과 함께 쓸 수 있다. */
      {
        uint8_t before[CHANNEL_COUNT];
        uint8_t c;

        for (c = 0U; c < CHANNEL_COUNT; ++c)
        {
          before[c] = channel_ctrl[c].applied_risk;
        }

        sched_tick();

        for (c = 0U; c < CHANNEL_COUNT; ++c)
        {
          if (channel_ctrl[c].applied_risk != before[c])
          {
            CHECK(channel_ctrl[c].applied_risk == last_sent_risk[c],
                  "applied changed to a value that was not just sent: "
                  "ch=%u %u -> %u, last sent %u",
                  c, before[c], channel_ctrl[c].applied_risk, last_sent_risk[c]);
          }
        }
      }
      check_safety();

      /* osDelayUntil 의 동작을 그대로 흉내낸다: 목표 시각이 이미 지났으면 재우지 않고
       * 즉시 다음 회차로 간다. 실기에서 ACK 타임아웃(100ms)이 틱(20ms)보다 길 때
       * 백투백으로 다음 채널을 잡는 것이 바로 이 경로다. */
      if (next_tick < fake_now)
      {
        next_tick = fake_now;
      }
      if (next_inject < fake_now)
      {
        next_inject = fake_now;
      }
    }
  }
}

/** 모든 채널을 "맞춰졌고 Slave 도 살아있다"로 되돌린다. */
static void reset_world(unsigned seed)
{
  uint8_t ch;

  g_seed = seed;
  srand(seed);
  fake_now = 100000U;   /* 틱이 충분히 흐른 뒤에서 시작 */
  sched_cursor = 0U;
  stat_rs485_tx_fail = 0U;
  stat_sched_retry = 0U;
  stat_sched_refresh = 0U;
  stat_lat_max = 0U;
  stat_lat_danger_max = 0U;
  stat_ack_slot_lost = 0U;
  tx_count = 0U;
  memset(tx_per_channel, 0, sizeof(tx_per_channel));
  memset(board, 0, sizeof(board));
  uplink_count = 0U;
  memset(ever_desired, 0, sizeof(ever_desired));
  memset(last_sent_risk, 0, sizeof(last_sent_risk));   /* 부팅 상태 = NONE */
  interleave_enabled = 0;   /* 켜는 것은 그 검사가 reset_world 뒤에 직접 한다 */

  for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
  {
    channel_ctrl[ch].desired_risk = VEDA_RISK_NONE;
    channel_ctrl[ch].applied_risk = VEDA_RISK_NONE;
    channel_ctrl[ch].last_tx_tick = fake_now;
    channel_ctrl[ch].retry = 0U;
    channel_ctrl[ch].desired_tick = fake_now;
    channel_ctrl[ch].lat_pending = 0U;
    channel_ctrl[ch].lat_risk = VEDA_RISK_NONE;
    ever_desired[ch][VEDA_RISK_NONE] = 1U;   /* 부팅 상태 = NONE */
    risk_to_status(VEDA_RISK_NONE, &channel_status[ch]);

    slave[ch].reachable = 1;
    slave[ch].will_ack = 1;
    slave[ch].applied = VEDA_RISK_NONE;
    slave_last_apply[ch] = fake_now;
    slave_max_gap[ch] = 0U;
  }
}

/** 전원을 막 켠 직후로 되돌린다. main.c 의 channel_boot_state_init() 과 같아야 한다. */
static void boot_world(unsigned seed)
{
  uint8_t ch;

  reset_world(seed);
  memset(channel_status, 0, sizeof(channel_status));   /* LED 전부 0 = 아직 확인 안 됨 */
  for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
  {
    channel_ctrl[ch].applied_risk = SCHED_RISK_UNKNOWN;
    channel_ctrl[ch].retry = 0U;
  }
}

/* --- 검사들 ------------------------------------------------------------------------- */

/**
 * 정상 상태에서 33ms 랜덤을 30초 퍼부은 뒤, 주입을 멈추면 모든 채널이 명령대로 수렴한다.
 * 이것이 "제대로 update 되는가"의 핵심 검사다. 수렴하지 않으면 어느 시드에서 어느 채널이
 * desired 와 어긋난 채 남았는지 찍고 멈춘다.
 */
static void test_healthy_soak_converges(void)
{
  unsigned s;
  for (s = 0U; s < SEED_COUNT; ++s)
  {
    uint8_t ch;
    reset_world(s);
    run(SOAK_MSEC, 1);      /* 랜덤 주입 */
    run(QUIET_MSEC, 0);     /* 조용해지면 따라잡을 시간을 준다 */

    for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
    {
      CHECK(channel_ctrl[ch].applied_risk == channel_ctrl[ch].desired_risk,
            "ch=%u did not converge: desired=%u applied=%u",
            ch, channel_ctrl[ch].desired_risk, channel_ctrl[ch].applied_risk);
      CHECK(slave[ch].applied == channel_ctrl[ch].desired_risk,
            "ch=%u slave out of sync: desired=%u slave=%u",
            ch, channel_ctrl[ch].desired_risk, slave[ch].applied);
    }
  }
}

/**
 * ACK 를 기다리는 '도중'에 desired 가 바뀌어도 상태가 오염되지 않고 결국 수렴한다.
 *
 * 왜 이 검사가 따로 필요한가: 위 test_healthy_soak_converges 를 포함한 모든 검사에서
 * inject_random() 은 sched_tick() '사이'에서만 돈다. 그래서 실기에서 가장 자주 일어나는
 * 동시성 상황 -- ctrl_task 가 ACK 대기 중인 sched_task 를 선점하는 것 -- 이 한 번도
 * 재현되지 않았다. ACK 대기는 최대 100ms 이고 RPi 명령은 그보다 촘촘할 수 있다.
 *
 * 노리는 고장은 하나다: **옛 명령의 ACK 가 그 사이 바뀐 새 상태를 '적용됐다'고 기록하는 것.**
 * 지금 코드는 세 겹으로 막고 있다 -- dispatch_channel() 이 want 를 스냅샷하고,
 * 보내기 직전 drain_ack_queue() 로 큐를 비우고, wait_for_slave_ack() 가 채널과 등급을
 * 둘 다 대조한다. 이 검사는 그 성질이 앞으로도 유지되는지를 지킨다.
 *
 * 실제 검출은 두 곳에서 일어난다:
 *   - run() 의 applied 전이 불변식 (매 틱, 오염을 즉시 잡는다)
 *   - 아래 수렴 검사 (주입을 멈춘 뒤 desired == applied == Slave 실제 상태)
 */
static void test_desired_changes_during_ack_wait(void)
{
  unsigned s;
  for (s = 0U; s < SEED_COUNT; ++s)
  {
    uint8_t ch;
    reset_world(s);
    interleave_enabled = 1;   /* reset_world 가 0 으로 되돌리므로 그 뒤에 켠다 */
    run(SOAK_MSEC, 1);        /* 틱 사이 주입 + ACK 대기 중 주입이 함께 돈다 */
    interleave_enabled = 0;   /* 조용한 구간에는 주입이 없어야 수렴을 볼 수 있다 */
    run(QUIET_MSEC, 0);

    for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
    {
      CHECK(channel_ctrl[ch].applied_risk == channel_ctrl[ch].desired_risk,
            "ch=%u did not converge after interleaved soak: desired=%u applied=%u",
            ch, channel_ctrl[ch].desired_risk, channel_ctrl[ch].applied_risk);
      CHECK(slave[ch].applied == channel_ctrl[ch].desired_risk,
            "ch=%u slave out of sync after interleaved soak: desired=%u slave=%u",
            ch, channel_ctrl[ch].desired_risk, slave[ch].applied);
    }
  }
}

/**
 * Slave 한 대(보드 1 = 채널 2,3)가 죽어 있어도, 살아 있는 보드(채널 0,1)의 채널은 계속
 * 갱신된다. 죽은 채널을 DANGER 로 고정해 DANGER 우선 패스를 독점시키는 최악의 상황을 만든다 --
 * SCHED_RETRY_BACKOFF_MSEC 가 없으면 여기서 살아 있는 채널이 굶어 수렴하지 못한다.
 * 동시에 죽은 채널은 ACK 가 없으므로 applied 가 절대 그 등급으로 바뀌지 않아야 한다
 * (Master 가 "정상 점등 중"이라고 잘못 보고하지 않는다).
 */
static void test_dead_board_isolation(void)
{
  unsigned s;
  for (s = 0U; s < SEED_COUNT; ++s)
  {
    reset_world(s);
    set_board_alive(1U, 0);       /* 채널 2,3 죽음 */
    run(SOAK_MSEC, 1);

    /* 최악의 스트레스: 죽은 채널을 DANGER 로, 산 채널은 새 값이 필요하게 만든다. */
    set_desired(2U, VEDA_RISK_DANGER);
    set_desired(3U, VEDA_RISK_DANGER);
    set_desired(0U, VEDA_RISK_WARNING);
    set_desired(1U, VEDA_RISK_DANGER);
    run(QUIET_MSEC, 0);

    CHECK(channel_ctrl[0].applied_risk == VEDA_RISK_WARNING,
          "live ch0 starved by dead DANGER channel: applied=%u", channel_ctrl[0].applied_risk);
    CHECK(channel_ctrl[1].applied_risk == VEDA_RISK_DANGER,
          "live ch1 starved by dead DANGER channel: applied=%u", channel_ctrl[1].applied_risk);
    CHECK(channel_ctrl[2].applied_risk == VEDA_RISK_NONE,
          "dead ch2 falsely reported applied=%u", channel_ctrl[2].applied_risk);
    CHECK(channel_ctrl[3].applied_risk == VEDA_RISK_NONE,
          "dead ch3 falsely reported applied=%u", channel_ctrl[3].applied_risk);
  }
}

/**
 * 죽었던 Slave 가 돌아오면 Master 가 RPi 개입 없이 스스로 상태를 맞춘다.
 * (SCHED_REFRESH_MSEC / 재시도가 복구를 맡는다는 것을 실제 dispatch 로 확인.)
 */
static void test_board_recovers(void)
{
  unsigned s;
  for (s = 0U; s < SEED_COUNT; ++s)
  {
    uint8_t ch;
    reset_world(s);
    set_board_alive(1U, 0);
    run(SOAK_MSEC / 2U, 1);

    set_board_alive(1U, 1);       /* 보드가 돌아왔다 */
    /* 알려진 값으로 고정해 정확한 수렴을 확인한다. */
    set_desired(0U, VEDA_RISK_DANGER);
    set_desired(1U, VEDA_RISK_NONE);
    set_desired(2U, VEDA_RISK_WARNING);
    set_desired(3U, VEDA_RISK_DANGER);
    run(QUIET_MSEC, 0);

    for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
    {
      CHECK(channel_ctrl[ch].applied_risk == channel_ctrl[ch].desired_risk,
            "ch=%u did not recover: desired=%u applied=%u",
            ch, channel_ctrl[ch].desired_risk, channel_ctrl[ch].applied_risk);
      CHECK(slave[ch].applied == channel_ctrl[ch].desired_risk,
            "ch=%u slave not resynced after recovery: desired=%u slave=%u",
            ch, channel_ctrl[ch].desired_risk, slave[ch].applied);
    }
  }
}

/**
 * Slave 가 조용히 리셋돼 표시를 잃어도(Master 는 여전히 맞다고 안다), 주기 리프레시가
 * SCHED_REFRESH_MSEC 안에 상태를 다시 밀어 넣는다. desired==applied 라 재시도 경로로는
 * 절대 안 잡히는 상황이라, 리프레시가 없으면 영영 복구되지 않는다.
 */
static void test_refresh_resyncs_silent_reset(void)
{
  uint8_t ch;

  reset_world(0U);
  for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
  {
    set_desired(ch, VEDA_RISK_DANGER);
  }
  run(500U, 0);   /* DANGER 로 수렴 */
  for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
  {
    CHECK(channel_ctrl[ch].applied_risk == VEDA_RISK_DANGER, "ch=%u setup failed", ch);
    CHECK(slave[ch].applied == VEDA_RISK_DANGER, "ch=%u setup slave failed", ch);
  }

  /* Slave 가 조용히 재부팅됐다: 표시는 사라졌지만 Master 는 여전히 DANGER 로 알고 있다. */
  for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
  {
    slave[ch].applied = VEDA_RISK_NONE;
  }

  run(SCHED_REFRESH_MSEC + 500U, 0);   /* 리프레시 주기를 한참 넘긴다 */

  for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
  {
    CHECK(slave[ch].applied == VEDA_RISK_DANGER,
          "ch=%u refresh did not resync silent reset: slave=%u", ch, slave[ch].applied);
  }
}

/**
 * 반응 지연 상한. 이 시스템이 경보 장비이므로 "언젠가 켜진다"가 아니라 "최악 몇 ms 안에
 * 켜진다"를 숫자로 못박는다. 육안으로는 20ms 와 80ms 를 구분할 수 없어서 실기 관찰로는
 * 절대 얻을 수 없는 값이다.
 *
 * 상한은 관측값에 여유를 얹어 정했다. 이 값을 넘기 시작하면 스케줄러가 느려진 것이므로
 * 무엇이 바뀌었는지 봐야 한다(틱 주기, 백오프, 우선순위 규칙, 채널 수).
 */
#define LAT_BOUND_HEALTHY_DANGER_MSEC 60U
#define LAT_BOUND_HEALTHY_ANY_MSEC 80U
#define LAT_BOUND_DEGRADED_DANGER_MSEC 400U
#define LAT_BOUND_DEGRADED_ANY_MSEC 600U

static void measure_latency(int degraded, uint32_t *out_any, uint32_t *out_danger)
{
  unsigned s;
  uint32_t worst_all = 0U;
  uint32_t worst_dgr = 0U;

  for (s = 0U; s < SEED_COUNT; ++s)
  {
    reset_world(s);
    if (degraded)
    {
      set_board_alive(1U, 0);   /* 보드 한 대가 죽은 채로 부하를 건다 */
    }
    run(SOAK_MSEC, 1);
    if (stat_lat_max > worst_all) worst_all = stat_lat_max;
    if (stat_lat_danger_max > worst_dgr) worst_dgr = stat_lat_danger_max;
  }
  *out_any = worst_all;
  *out_danger = worst_dgr;
}

/**
 * DANGER 는 경보를 '켜는' 명령이라 다른 등급보다 먼저 나가야 한다. 그 우선순위가 실제로
 * 지연에 반영되는지까지 함께 본다 -- lat_danger 가 lat_any 보다 크면 우선순위 패스가
 * 제 역할을 못 하고 있다는 뜻이고, 그건 규칙이 깨졌다는 신호다.
 */
static void test_latency_bounds(void)
{
  uint32_t any;
  uint32_t dgr;

  measure_latency(0, &any, &dgr);
  printf("  latency healthy : any=%3ums danger=%3ums\n", (unsigned)any, (unsigned)dgr);
  CHECK(dgr <= LAT_BOUND_HEALTHY_DANGER_MSEC,
        "healthy DANGER latency %ums exceeds bound %ums", any, LAT_BOUND_HEALTHY_DANGER_MSEC);
  CHECK(any <= LAT_BOUND_HEALTHY_ANY_MSEC,
        "healthy latency %ums exceeds bound %ums", any, LAT_BOUND_HEALTHY_ANY_MSEC);
  CHECK(dgr <= any, "DANGER slower than overall max -- priority pass broken");

  measure_latency(1, &any, &dgr);
  printf("  latency degraded: any=%3ums danger=%3ums (board down)\n",
         (unsigned)any, (unsigned)dgr);
  CHECK(dgr <= LAT_BOUND_DEGRADED_DANGER_MSEC,
        "degraded DANGER latency %ums exceeds bound %ums", dgr, LAT_BOUND_DEGRADED_DANGER_MSEC);
  CHECK(any <= LAT_BOUND_DEGRADED_ANY_MSEC,
        "degraded latency %ums exceeds bound %ums", any, LAT_BOUND_DEGRADED_ANY_MSEC);
}

/**
 * 같은 등급이 반복해서 와도 회선에는 한 번만 나가야 한다(코얼레싱). 이게 깨지면 예전의
 * 버스 백로그 버그가 부활한다 -- DANGER 연발이 전부 회선에 쌓여 뒤늦게 몰아서 재생됐다.
 * 리프레시는 시간 기반이라 따로 세어 빼 준다.
 */
static void test_coalescing_bounds_traffic(void)
{
  uint32_t before;
  int i;

  reset_world(0U);
  run(200U, 0);                       /* 초기 상태를 안정시킨다 */
  before = tx_count;

  /* 같은 값을 100번 주입한다. 회선에 나가는 것은 '값이 바뀐' 한 번뿐이어야 한다. */
  for (i = 0; i < 100; ++i)
  {
    set_desired(0U, VEDA_RISK_DANGER);
    run(SCHED_TICK_MSEC, 0);
  }

  /* 100회 주입 + 2초 구간이라 리프레시도 몇 번 섞인다. 채널 4개 × 리프레시 몇 회를
   * 감안해도 20회를 넘으면 코얼레싱이 동작하지 않는 것이다(100회가 그대로 나간 경우
   * 이 값은 100을 넘는다). */
  CHECK((tx_count - before) <= 20U,
        "coalescing failed: %u transmissions for 100 repeated commands",
        (unsigned)(tx_count - before));
}

/**
 * 주기 리프레시가 최악 조건에서도 실제로 도는지 본다.
 *
 * Slave 에는 링크 두절 failsafe 가 없다(fail-loud -- 마지막 상태를 유지한다). 그래서
 * Slave 가 리셋되거나 프레임을 놓쳐 상태가 어긋났을 때 **되돌리는 경로는 이 리프레시
 * 하나뿐**이다. 리프레시가 굶으면 어긋난 경광등이 영영 어긋난 채로 남는다.
 *
 * 최악 조건으로 건다: 보드 한 대가 죽어 재시도가 계속 돌고, 그 와중에 랜덤 명령이 쏟아진다.
 * 이때 살아 있는 보드의 채널이 얼마나 오래 방치되는지가 답이다.
 *
 * SCHED_RETRY_BACKOFF_MSEC 를 잘못 줄이면 죽은 DANGER 채널이 우선순위 패스를 독점해
 * 이 값이 먼저 터진다 -- 굶주림 회귀 검사를 겸한다.
 */
static void test_refresh_period_guarantee(void)
{
  unsigned s;
  uint8_t ch;
  uint32_t worst_stress = 0U;
  uint32_t worst_quiet = 0U;

  /* --- 1) 부하 구간: 보드 한 대 사망 + 랜덤 명령이 쏟아지는 최악 조건 --------------- */
  for (s = 0U; s < SEED_COUNT; ++s)
  {
    reset_world(s);
    set_board_alive(1U, 0);        /* 보드 1(채널 2,3) 사망 -- 재시도가 계속 돈다 */
    run(SOAK_MSEC, 1);

    for (ch = 0U; ch < CHANNELS_PER_SLAVE; ++ch)   /* 살아 있는 보드의 채널만 본다 */
    {
      if (slave_max_gap[ch] > worst_stress)
      {
        worst_stress = slave_max_gap[ch];
      }
    }
  }

  /* --- 2) 정적 구간: 새 명령이 하나도 없을 때 -------------------------------------
   * !! 이 두 번째 측정이 없으면 리프레시 약화를 잡지 못한다. 부하 구간에서는 33ms 랜덤
   *    명령이 채널을 계속 건드려서, 리프레시가 아예 죽어 있어도 공백이 벌어지지 않는다
   *    (뮤테이션 검사에서 SCHED_REFRESH_MSEC 를 2000 -> 20000 으로 바꿔도 통과했다).
   *    리프레시의 진짜 주기는 회선이 조용할 때만 드러난다 -- 그리고 Slave 에 failsafe 가
   *    없는 지금, 조용할 때 상태를 맞춰 주는 것이 정확히 이 경로다. */
  reset_world(0U);
  run(500U, 0);                    /* 초기 상태를 안정시킨다 */
  for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
  {
    slave_last_apply[ch] = fake_now;
    slave_max_gap[ch] = 0U;
  }
  run(10000U, 0);                  /* 명령 없이 10초 -- 리프레시만 돈다 */

  for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
  {
    /* !! 아직 안 끝난 공백도 세야 한다. slave_max_gap 은 '송신이 일어난 순간'에만 갱신되므로,
     *    리프레시가 아예 안 오면 기록이 0 으로 남아 통과해 버린다(뮤테이션 검사에서 이것
     *    때문에 리프레시 20000ms 가 살아남았다). 창이 끝난 시점의 미결 공백을 함께 본다. */
    const uint32_t pending = fake_now - slave_last_apply[ch];
    const uint32_t worst_ch = (slave_max_gap[ch] > pending) ? slave_max_gap[ch] : pending;

    if (worst_ch > worst_quiet)
    {
      worst_quiet = worst_ch;
    }
  }

  printf("  refresh gap: stress=%ums quiet=%ums   bound %ums\n",
         (unsigned)worst_stress, (unsigned)worst_quiet, (unsigned)REFRESH_GAP_BOUND_MSEC);

  CHECK(worst_stress <= REFRESH_GAP_BOUND_MSEC,
        "under load a live channel went %ums without a command (bound %ums)",
        worst_stress, REFRESH_GAP_BOUND_MSEC);
  CHECK(worst_quiet <= REFRESH_GAP_BOUND_MSEC,
        "when idle a channel went %ums without a refresh (bound %ums) -- "
        "a desynced slave would stay wrong that long",
        worst_quiet, REFRESH_GAP_BOUND_MSEC);
}

/**
 * DANGER 우선 패스가 sched_tick() 안에서 실제로 도는지 본다.
 *
 * test_sched_select.c 도 우선순위를 검사하지만, 그건 pick_pending() 을 직접 부르고 두 패스를
 * 테스트가 흉내낸 것이다. 정작 "sched_tick() 이 1차 패스를 부르는가"는 아무도 확인하지
 * 않았다 -- 그 호출을 지워도 두 테스트가 모두 통과했다(뮤테이션 검사에서 드러난 구멍).
 *
 * 커서 앞쪽에 WARNING 을, 뒤쪽에 DANGER 를 둔다. 순서만 보면 CH0 이 먼저다.
 * 우선순위가 순서를 이겨야 한다.
 */
static void test_danger_dispatched_first(void)
{
  reset_world(0U);
  run(200U, 0);                    /* 전 채널을 맞춰 둔 상태에서 시작 */

  sched_cursor = 0U;
  set_desired(0U, VEDA_RISK_WARNING);
  set_desired(3U, VEDA_RISK_DANGER);

  sched_tick();                    /* 딱 한 번 */

  CHECK(slave[3].applied == VEDA_RISK_DANGER,
        "DANGER (ch3) was not dispatched first -- priority pass is not running");
  CHECK(slave[0].applied != VEDA_RISK_WARNING,
        "WARNING (ch0) went out before the pending DANGER");
}

/**
 * 커서가 전진해서 경쟁하는 채널이 번갈아 서비스되는지 본다.
 *
 * 커서가 없으면(매번 0부터 훑으면) 앞 채널이 계속 밀릴 때 뒤 채널이 굶는다. 이것이 커서의
 * 존재 이유인데, `sched_tick()` 의 커서 전진을 지워도 기존 검사가 전부 통과했다 --
 * 한 번에 한 채널만 어긋나 있으면 커서가 없어도 결국 다 처리되기 때문이다.
 * 그래서 **둘 이상이 동시에 계속 어긋난** 상황을 만들어야 한다.
 *
 * CH0 과 CH3 을 매 틱 서로 다른 값으로 뒤집어 항상 어긋나게 두고, 둘이 고르게 나가는지 본다.
 * DANGER 는 쓰지 않는다 -- 우선순위 패스가 끼면 커서와 무관하게 잡히기 때문이다.
 */
static void test_cursor_advances_between_contenders(void)
{
  int i;
  uint32_t a;
  uint32_t b;

  reset_world(0U);
  run(200U, 0);
  memset(tx_per_channel, 0, sizeof(tx_per_channel));

  for (i = 0; i < 200; ++i)
  {
    /* 두 채널 모두 매번 새 값으로 -- 항상 desired != applied 인 상태를 유지한다. */
    const uint8_t v = (uint8_t)(((i % 2) == 0) ? VEDA_RISK_WARNING : VEDA_RISK_NONE);
    set_desired(0U, v);
    set_desired(3U, v);
    sched_tick();
  }

  a = tx_per_channel[0];
  b = tx_per_channel[3];
  CHECK(a > 0U && b > 0U, "one contender never served: ch0=%u ch3=%u", a, b);
  /* 완전히 균등할 필요는 없지만 한쪽이 다른 쪽의 3배를 넘으면 커서가 안 돈다는 뜻이다. */
  CHECK(a <= b * 3U && b <= a * 3U,
        "cursor is not advancing -- unfair service: ch0=%u ch3=%u", a, b);
}

/**
 * 부팅: 확인되기 전에는 초록이라고 말하지 않고, 확인된 그 순간 반드시 한 장 올려 보낸다.
 *
 * 이 검사가 지키는 것은 Qt 화면이다. RPi(SerialHwEventDispatcher::reportIndicators)도 Qt 도
 * 상행 값이 '바뀔 때만' 갱신하므로, Master 가 부팅 순간부터 초록을 싣고 올라가면 전이가 한
 * 번도 생기지 않아 화면에는 아무 것도 반영되지 않는다(실제로 그랬다). 부팅값이 '미확인'이고
 * Slave 의 ACK 로 확인될 때 초록으로 바뀌어야, 그 전이 한 번이 Qt 까지 올라간다.
 *
 * 동시에 정직성도 함께 건다 -- 아직 응답하지 않은 보드의 채널을 초록이라고 우기면 안 된다.
 */
static void test_boot_reports_only_confirmed_state(void)
{
  uint8_t ch;
  uint32_t uplinks_before;

  /* --- 1) 보드 1(채널 2,3)은 아직 부팅 중이라 응답하지 않는다 ---------------------- */
  boot_world(0U);
  set_board_alive(1U, 0);

  for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
  {
    CHECK(channel_status[ch].led_green == 0U,
          "ch=%u claimed green before any ACK -- boot state is a guess, not a report", ch);
  }

  uplinks_before = uplink_count;
  run(1000U, 0);

  CHECK(channel_ctrl[0].applied_risk == VEDA_RISK_NONE,
        "live ch0 never confirmed at boot: applied=%u", channel_ctrl[0].applied_risk);
  CHECK(channel_status[0].led_green == 1U,
        "live ch0 confirmed but never reported green -- Qt would still show nothing");
  CHECK(uplink_count > uplinks_before,
        "no uplink was queued for the boot state -- the transition never reaches Qt");

  CHECK(channel_ctrl[2].applied_risk == SCHED_RISK_UNKNOWN,
        "ch2 marked confirmed without an ACK: applied=%u", channel_ctrl[2].applied_risk);
  CHECK(channel_status[2].led_green == 0U,
        "ch2 reported green while its board was still down");

  /* --- 2) 늦게 뜬 보드도 재시도/리프레시가 확인해 준다 ------------------------------ */
  uplinks_before = uplink_count;
  set_board_alive(1U, 1);
  run(SCHED_REFRESH_MSEC + 1000U, 0);

  for (ch = 0U; ch < CHANNEL_COUNT; ++ch)
  {
    CHECK(channel_ctrl[ch].applied_risk == VEDA_RISK_NONE,
          "ch=%u never converged after its board came up: applied=%u",
          ch, channel_ctrl[ch].applied_risk);
    CHECK(channel_status[ch].led_green == 1U,
          "ch=%u confirmed but not reported green", ch);
  }
  CHECK(uplink_count > uplinks_before,
        "late board's confirmation was never reported upward");
}

int main(void)
{
  test_boot_reports_only_confirmed_state();
  test_latency_bounds();
  test_refresh_period_guarantee();
  test_danger_dispatched_first();
  test_cursor_advances_between_contenders();
  test_coalescing_bounds_traffic();
  test_healthy_soak_converges();
  test_desired_changes_during_ack_wait();
  test_dead_board_isolation();
  test_board_recovers();
  test_refresh_resyncs_silent_reset();

  printf("sched_loop: all checks passed (%u seeds x %u ms soak)\n",
         (unsigned)SEED_COUNT, (unsigned)SOAK_MSEC);
  return 0;
}
