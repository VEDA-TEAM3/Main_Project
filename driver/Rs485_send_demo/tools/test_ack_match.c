/**
 * @file test_ack_match.c
 * @brief ack_match.inc — "이 ACK 가 내가 기다리던 답인가" 판별을 호스트에서 검사한다.
 *
 * 빌드/실행 (tools/ 에서):
 *   gcc -Wall -Wextra -I../Core/Inc test_ack_match.c -o test_ack_match && ./test_ack_match
 *
 * 검사 B(test_ack_parse.c)와 무엇이 다른가:
 *   B 는 회선에서 온 글자가 "형식상 ACK 인가"를 본다. 여기는 그 다음 질문을 본다 --
 *   큐에 들어온 ACK 가 **방금 보낸 명령에 대한 답이 맞는가**.
 *
 * 왜 이 층이 필요한가:
 *   이 판별은 큐와 시간이 얽혀 있어 실기에서 재현이 거의 불가능하다. 늦게 도착한 ACK 를
 *   이번 명령의 확인으로 오인하는 순간 **적용되지 않은 명령이 적용된 것으로 기록**되는데,
 *   그건 밖에서 보면 그냥 "잘 되고 있다"로 보인다. 조용히 틀리는 종류의 고장이다.
 *
 * 가짜 seam 두 개(ack_queue_take / ack_now_ms)로 ACK 도착 시각까지 대본으로 정한다.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- main.c 에서 가져온 최소한의 문맥 ------------------------------------- */
#define RS485_ACK_REQUIRED 1
#define RS485_ACK_TIMEOUT_MSEC 100U

typedef struct
{
  uint8_t slave;
  uint8_t channel;      /* 1-based (회선 위 표현) */
  uint8_t risk_level;
} rs485_ack_t;

static uint32_t stat_ack_timeout;
static uint32_t stat_ack_mismatch;

/* --- 가짜 시계 ------------------------------------------------------------ */
static uint32_t fake_now;
static uint32_t ack_now_ms(void) { return fake_now; }

/* --- 가짜 ACK 큐: 도착 시각까지 대본으로 정한다 ---------------------------- */
typedef struct
{
  uint32_t    arrive_ms;  /* 이 시각이 되어야 큐에서 꺼낼 수 있다 */
  rs485_ack_t ack;
} scripted_ack_t;

#define SCRIPT_MAX 8
static scripted_ack_t script[SCRIPT_MAX];
static int script_len;
static int script_pos;

/** 대본을 비우고 시계를 되돌린다. */
static void reset_script(void)
{
  script_len = 0;
  script_pos = 0;
  fake_now = 1000U;          /* 0 이 아닌 곳에서 시작해 뺄셈 실수를 드러낸다 */
  stat_ack_timeout = 0U;
  stat_ack_mismatch = 0U;
}

/** "t 시각에 S<slave>:CH<ch>:R<risk> ACK 가 큐에 들어온다" 를 예약한다. */
static void schedule_ack(uint32_t at_ms, uint8_t slave, uint8_t ch, uint8_t risk)
{
  script[script_len].arrive_ms = at_ms;
  script[script_len].ack.slave = slave;
  script[script_len].ack.channel = ch;
  script[script_len].ack.risk_level = risk;
  ++script_len;
}

/**
 * 가짜 큐. 대본의 다음 ACK 가 timeout 안에 도착하면 그 시각까지 시계를 당기고 넘겨준다.
 * 아니면 timeout 만큼 시계를 흘리고 "없음"을 돌려준다 -- 실제 osMessageQueueGet 과 같다.
 */
static uint8_t ack_queue_take(rs485_ack_t *out, uint32_t timeout_ms)
{
  if (script_pos < script_len)
  {
    /* 부호 없는 뺄셈으로 '얼마나 남았나'를 잰다. 검사 대상(wait_for_slave_ack)이 감김에
     * 안전한지 보려면 가짜 큐부터 감김에 안전해야 한다 -- 여기서 평범한 <= 비교를 쓰면
     * 감김 검사가 가짜 쪽 한계 때문에 무의미해진다.
     * delta 가 절반을 넘으면 '과거'로 본다(이미 큐에 쌓여 있던 것). */
    const uint32_t delta = script[script_pos].arrive_ms - fake_now;

    if (delta == 0U || delta > 0x80000000U)
    {
      *out = script[script_pos].ack;   /* 이미 큐에 있던 것(늦은 ACK) */
      ++script_pos;
      return 1U;
    }
    if (delta <= timeout_ms)
    {
      fake_now += delta;               /* 기다리다 받았다 */
      *out = script[script_pos].ack;
      ++script_pos;
      return 1U;
    }
  }
  fake_now += timeout_ms;              /* 끝까지 기다렸지만 없다 */
  return 0U;
}

#include "ack_match.inc"

/* --- 검사 도구 ------------------------------------------------------------ */
#define CHECK(cond, ...)                             \
  do {                                               \
    if (!(cond)) {                                   \
      fprintf(stderr, "FAIL (t=%ums): ", fake_now);  \
      fprintf(stderr, __VA_ARGS__);                  \
      fprintf(stderr, "\n");                         \
      abort();                                       \
    }                                                \
  } while (0)

/* --- 검사들 --------------------------------------------------------------- */

/** 기다리던 ACK 가 제때 오면 성공한다. 채널은 0-based 를 1-based 로 맞춰 비교해야 한다. */
static void test_matching_ack_accepted(void)
{
  reset_script();
  schedule_ack(1010U, 1U, 1U, 2U);        /* CH1 에 R2 */

  CHECK(wait_for_slave_ack(0U, 2U) == 1U, "matching ACK was not accepted");
  CHECK(stat_ack_timeout == 0U, "timeout counted on success");
  CHECK(stat_ack_mismatch == 0U, "mismatch counted on success");
  CHECK(fake_now == 1010U, "clock did not advance to arrival (%ums)", fake_now);
}

/**
 * 다른 채널의 ACK 가 먼저 와도 실패로 처리하면 안 된다 -- 버스를 공유하므로 옆 채널의
 * ACK 는 정상적으로 섞여 들어온다. 그걸로 포기하면 멀쩡한 명령이 매번 재전송된다.
 */
static void test_other_channel_ack_does_not_abort(void)
{
  reset_script();
  schedule_ack(1005U, 2U, 3U, 2U);        /* 옆 채널 ACK 가 먼저 */
  schedule_ack(1020U, 1U, 1U, 2U);        /* 내 ACK 는 그 뒤에 */

  CHECK(wait_for_slave_ack(0U, 2U) == 1U, "gave up on someone else's ACK");
  CHECK(stat_ack_mismatch == 1U, "mismatch not counted (%u)", stat_ack_mismatch);
  CHECK(stat_ack_timeout == 0U, "timeout wrongly counted");
}

/**
 * 채널은 맞는데 등급이 다르면 내 답이 아니다.
 * WARNING 을 보냈는데 DANGER 가 적용됐다고 오면 그건 확인이 아니라 불일치다.
 */
static void test_wrong_risk_is_not_accepted(void)
{
  reset_script();
  schedule_ack(1005U, 1U, 1U, 2U);        /* CH1 인데 등급이 다르다 */

  CHECK(wait_for_slave_ack(0U, 1U) == 0U, "wrong risk level was accepted as confirmation");
  CHECK(stat_ack_mismatch == 1U, "mismatch not counted");
  CHECK(stat_ack_timeout == 1U, "timeout not counted after mismatch drought");
}

/**
 * !! 이 검사가 이 파일의 존재 이유다.
 * 앞 명령의 ACK 가 큐에 남아 있는 채로 다음 명령을 보내면, 그 늦은 ACK 가 **내용까지
 * 우연히 같을 수 있다**(같은 채널을 같은 등급으로 재전송하는 경우 -- 재시도와 리프레시가
 * 정확히 그렇다). 그러면 보내지도 않은 확인을 받은 셈이 되어 적용되지 않은 명령이
 * 적용된 것으로 기록된다.
 * 막는 방법은 하나뿐이다: 보내기 직전에 큐를 비운다.
 */
static void test_stale_ack_is_drained_before_send(void)
{
  reset_script();
  schedule_ack(1000U, 1U, 1U, 2U);        /* 앞 명령의 ACK 가 이미 큐에 있다 */
  schedule_ack(1200U, 1U, 1U, 2U);        /* 이번 명령의 진짜 ACK 는 한참 뒤(타임아웃 밖) */

  drain_ack_queue();                      /* 실기의 dispatch_channel() 이 하는 일 */

  /* 큐를 비웠으므로 남은 것은 200ms 뒤의 ACK 뿐 -- 타임아웃(100ms)을 넘으니 실패해야 한다. */
  CHECK(wait_for_slave_ack(0U, 2U) == 0U,
        "stale ACK was accepted as confirmation -- drain_ack_queue() is not working");
  CHECK(stat_ack_timeout == 1U, "timeout not counted");
}

/** drain 이 큐를 정말 비우는지. 여러 장이 쌓여 있어도 전부 없어져야 한다. */
static void test_drain_empties_everything(void)
{
  rs485_ack_t leftover;

  reset_script();
  schedule_ack(1000U, 1U, 1U, 0U);
  schedule_ack(1000U, 1U, 2U, 1U);
  schedule_ack(1000U, 2U, 3U, 2U);

  drain_ack_queue();
  CHECK(ack_queue_take(&leftover, 0U) == 0U, "queue still had entries after drain");
}

/** 아무것도 안 오면 타임아웃이고, 시계는 정확히 타임아웃만큼만 흘러야 한다. */
static void test_silence_times_out(void)
{
  reset_script();

  CHECK(wait_for_slave_ack(0U, 2U) == 0U, "silence was treated as success");
  CHECK(stat_ack_timeout == 1U, "timeout not counted");
  CHECK(fake_now == 1000U + RS485_ACK_TIMEOUT_MSEC,
        "clock advanced by %ums, expected %ums", fake_now - 1000U, RS485_ACK_TIMEOUT_MSEC);
}

/**
 * 남의 ACK 가 잇달아 와서 시간을 다 까먹으면 결국 타임아웃이어야 한다.
 * "계속 기다린다"가 "영원히 기다린다"가 되면 sched_task 가 그 자리에서 멈춘다.
 */
static void test_mismatch_flood_still_times_out(void)
{
  reset_script();
  schedule_ack(1020U, 2U, 3U, 2U);
  schedule_ack(1040U, 2U, 4U, 1U);
  schedule_ack(1060U, 2U, 3U, 0U);
  schedule_ack(1080U, 2U, 4U, 2U);
  /* 내 ACK 는 영영 오지 않는다 */

  CHECK(wait_for_slave_ack(0U, 2U) == 0U, "flood of mismatches was accepted");
  CHECK(stat_ack_mismatch == 4U, "mismatch count %u, expected 4", stat_ack_mismatch);
  CHECK(stat_ack_timeout == 1U, "timeout not counted");
  CHECK(fake_now <= 1000U + RS485_ACK_TIMEOUT_MSEC,
        "waited %ums, longer than the timeout -- sched_task would stall",
        fake_now - 1000U);
}

/**
 * 시각이 감기는 순간(49.7일)에도 경과 시간 계산이 정확해야 한다.
 * 평범한 뺄셈이나 비교를 쓰면 여기서 경과가 40억ms 로 보여 **첫 회차에 바로 타임아웃**이
 * 난다 -- 49.7일에 한 번, 모든 채널이 동시에 재전송에 들어가는 형태로 나타난다.
 */
static void test_clock_wraparound(void)
{
  reset_script();
  fake_now = 0xFFFFFFC0U;                 /* 64ms 뒤에 감긴다 */
  schedule_ack(0x00000010U, 1U, 1U, 2U);  /* 감긴 뒤 도착 -- 실제 경과 80ms, 타임아웃 안 */

  CHECK(wait_for_slave_ack(0U, 2U) == 1U,
        "wrapped clock broke the match -- elapsed time is not using unsigned subtraction");
  CHECK(stat_ack_timeout == 0U, "timeout wrongly counted across the wrap");
  CHECK(fake_now == 0x00000010U, "clock landed at %u, expected 0x10", fake_now);
}

/** 감김 직후 아무것도 안 와도 타임아웃이 정확히 한 번만 세어져야 한다. */
static void test_wraparound_timeout(void)
{
  reset_script();
  fake_now = 0xFFFFFFE0U;                 /* 32ms 뒤 감김 */

  CHECK(wait_for_slave_ack(0U, 2U) == 0U, "silence across wrap treated as success");
  CHECK(stat_ack_timeout == 1U, "timeout counted %u times, expected 1", stat_ack_timeout);
}

/**
 * !! 감김 검사의 핵심. 앞의 두 검사는 한 번 만에 끝나서 '경과 시간을 다시 계산하는 경로'를
 *    타지 않는다 -- 실제로 경과 계산을 망가뜨린 사본이 그 둘을 통과했다.
 *
 * 감김을 넘어 **불일치 ACK 가 잇달아** 들어오게 해서 루프를 여러 번 돌린다.
 * 경과 계산이 감김에 취약하면(예: now > start 일 때만 빼고 아니면 0) 매 회차 예산이
 * 100ms 로 되살아나 **영원히 기다린다** -- sched_task 가 그 자리에서 멈추고,
 * 49.7일에 한 번 모든 채널이 동시에 굳는 형태로 나타난다.
 */
static void test_wraparound_mismatch_flood(void)
{
  const uint32_t start = 0xFFFFFFC0U;     /* 64ms 뒤 감김 */
  uint32_t waited;

  reset_script();
  fake_now = start;

  /* 전부 남의 채널 -- 내 ACK 는 영영 오지 않는다. 감김 전후로 걸쳐 있다. */
  schedule_ack(0xFFFFFFD0U, 2U, 3U, 2U);  /* +16ms (감김 전) */
  schedule_ack(0xFFFFFFF0U, 2U, 4U, 1U);  /* +48ms (감김 전) */
  schedule_ack(0x00000000U, 2U, 3U, 0U);  /* +64ms (감김 직후) */
  schedule_ack(0x00000010U, 2U, 4U, 2U);  /* +80ms */
  schedule_ack(0x00000020U, 2U, 3U, 1U);  /* +96ms */

  CHECK(wait_for_slave_ack(0U, 2U) == 0U, "mismatch flood across wrap was accepted");

  /* 부호 없는 뺄셈으로 재야 감김을 넘어도 정확하다. */
  waited = fake_now - start;
  CHECK(waited <= RS485_ACK_TIMEOUT_MSEC,
        "waited %ums across the wrap, longer than the %ums timeout -- "
        "elapsed time is not using unsigned subtraction, sched_task would stall",
        waited, RS485_ACK_TIMEOUT_MSEC);
  CHECK(stat_ack_timeout == 1U, "timeout not counted");
}

int main(void)
{
  test_matching_ack_accepted();
  test_other_channel_ack_does_not_abort();
  test_wrong_risk_is_not_accepted();
  test_stale_ack_is_drained_before_send();
  test_drain_empties_everything();
  test_silence_times_out();
  test_mismatch_flood_still_times_out();
  test_clock_wraparound();
  test_wraparound_timeout();
  test_wraparound_mismatch_flood();

  printf("ack_match: all checks passed\n");
  return 0;
}
