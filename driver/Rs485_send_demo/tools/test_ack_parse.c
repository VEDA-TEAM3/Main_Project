/**
 * @file test_ack_parse.c
 * @brief ack_parse.inc 의 ACK 줄 조립·해석 규칙을 호스트에서 검사한다.
 *
 * 빌드/실행 (tools/ 에서):
 *   gcc -Wall -Wextra -I../Core/Inc test_ack_parse.c -o test_ack_parse && ./test_ack_parse
 *
 * 왜 이 층이 필요한가:
 *   이 코드는 USART1 수신 ISR 안에서 돈다. 실기에서 관찰하려고 로그를 찍으면 그 자체로
 *   한 바이트 시간(115200 기준 약 87us)을 넘겨 오버런이 나므로, 회선 위에서 무슨 일이
 *   있었는지 사실상 볼 수 없다. 반면 하는 일은 순수한 문자열 판별이라 호스트에서 전부
 *   재현할 수 있다 -- 굳이 굽지 않아도 되는 대표적인 코드다.
 *
 *   실기에서 실제로 관찰된 두 가지가 여기 검사로 들어와 있다:
 *     1) 버스를 공유하므로 Slave #2 가 Slave #1 의 ACK를 엿듣는다. Master 도 마찬가지로
 *        모든 ACK를 듣는데, 그걸 형식 오류로 세면 안 된다(정상 프레임이다).
 *     2) 자동 방향 전환 트랜시버는 자기 송신을 되울린다. 우리가 보낸 "S..." 명령이
 *        되돌아오는데, 이것을 ack_bad 로 세면 명령마다 카운터가 올라가 진짜 오류를 가린다.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- main.c 에서 가져온 최소한의 문맥 ------------------------------------- */
#define RS485_ACK_LINE_BUFFER_SIZE 16U

typedef struct
{
  uint8_t slave;
  uint8_t channel;
  uint8_t risk_level;
} rs485_ack_t;

static char    ack_line[RS485_ACK_LINE_BUFFER_SIZE];
static uint8_t ack_line_length;
static uint8_t ack_line_overflow;

static uint32_t stat_ack_ok;
static uint32_t stat_ack_bad;

/** 큐 대신 마지막 한 장만 붙잡아 둔다. 몇 장이 들어왔는지도 함께 센다. */
static rs485_ack_t last_ack;
static uint32_t    queued_count;

static void ack_queue_put(const rs485_ack_t *ack)
{
  last_ack = *ack;
  ++queued_count;
}

#include "ack_parse.inc"

/* --- 검사 도구 ------------------------------------------------------------ */
static void reset_parser(void)
{
  ack_line_length = 0U;
  ack_line_overflow = 0U;
  stat_ack_ok = 0U;
  stat_ack_bad = 0U;
  queued_count = 0U;
  memset(&last_ack, 0, sizeof(last_ack));
}

/** 문자열을 한 바이트씩 흘려 넣는다. ISR 이 바이트 단위로 부르는 것과 같은 경로다. */
static void feed(const char *text)
{
  size_t i;
  for (i = 0U; text[i] != '\0'; ++i)
  {
    ack_feed_byte((uint8_t)text[i]);
  }
}

#define CHECK(cond, ...)                                     \
  do {                                                       \
    if (!(cond)) {                                           \
      fprintf(stderr, "FAIL: ");                             \
      fprintf(stderr, __VA_ARGS__);                          \
      fprintf(stderr, "\n");                                 \
      assert(0);                                             \
    }                                                        \
  } while (0)

/* --- 검사들 --------------------------------------------------------------- */

/** 정상 ACK 한 장이 그대로 해석되는가. */
static void test_valid_ack(void)
{
  reset_parser();
  feed("A1:CH1:R2\n");

  CHECK(stat_ack_ok == 1U, "ok=%u, expected 1", stat_ack_ok);
  CHECK(stat_ack_bad == 0U, "bad=%u, expected 0", stat_ack_bad);
  CHECK(queued_count == 1U, "queued=%u, expected 1", queued_count);
  CHECK(last_ack.slave == 1U && last_ack.channel == 1U && last_ack.risk_level == 2U,
        "parsed S%u CH%u R%u", last_ack.slave, last_ack.channel, last_ack.risk_level);
}

/** 두 자리 슬레이브/채널과 세 등급이 모두 통과하는가. */
static void test_all_valid_combinations(void)
{
  char buf[16];
  uint8_t s;
  uint8_t c;
  uint8_t r;

  reset_parser();
  for (s = 1U; s <= 9U; ++s)
  {
    for (c = 1U; c <= 9U; ++c)
    {
      for (r = 0U; r <= 2U; ++r)
      {
        sprintf(buf, "A%u:CH%u:R%u\n", s, c, r);
        feed(buf);
        CHECK(last_ack.slave == s && last_ack.channel == c && last_ack.risk_level == r,
              "roundtrip failed for A%u:CH%u:R%u", s, c, r);
      }
    }
  }
  CHECK(stat_ack_bad == 0U, "bad=%u on all-valid sweep", stat_ack_bad);
  CHECK(stat_ack_ok == 9U * 9U * 3U, "ok=%u, expected %u", stat_ack_ok, 9U * 9U * 3U);
}

/**
 * 우리가 보낸 명령이 되울려 들어와도 조용히 버려야 한다(ack_bad 로 세면 안 된다).
 * 이걸 세면 명령 하나마다 카운터가 올라가 진짜 형식 오류를 완전히 가려 버린다.
 */
static void test_own_command_echo_is_silent(void)
{
  reset_parser();
  feed("S1:CH1:R2\n");
  feed("S2:CH3:R0\n");

  CHECK(stat_ack_ok == 0U, "echo counted as ok");
  CHECK(stat_ack_bad == 0U, "echo counted as bad (%u) -- would mask real errors", stat_ack_bad);
  CHECK(queued_count == 0U, "echo was queued as an ACK");
}

/**
 * 버스를 공유하므로 다른 채널 앞으로 간 ACK도 우리 귀에 들어온다. 형식이 맞으면
 * 정상 ACK로 받아야 한다 -- 어느 명령에 대한 것인지 가리는 일은 상위(wait_for_slave_ack)
 * 의 몫이고, 여기서 버리면 그 판단 자체가 불가능해진다.
 */
static void test_other_channel_ack_is_accepted(void)
{
  reset_parser();
  feed("A1:CH2:R0\n");

  CHECK(stat_ack_ok == 1U, "cross-talk ACK rejected");
  CHECK(stat_ack_bad == 0U, "cross-talk ACK counted bad");
  CHECK(last_ack.channel == 2U, "wrong channel parsed");
}

/** 형식이 어긋난 줄은 전부 ack_bad 로 세고 버려야 한다. */
static void test_malformed_lines_rejected(void)
{
  static const char *const bad[] = {
    "A1:CH1:R3\n",    /* 등급이 규약 밖(3) */
    "A1:CH1:R9\n",    /* 등급이 규약 밖(9) */
    "A0:CH1:R1\n",    /* 슬레이브 0 -- 1-based 규약 위반 */
    "A1:CH0:R1\n",    /* 채널 0 -- 1-based 규약 위반 */
    "A1:CH1:R\n",     /* 너무 짧다 */
    "A1:CH1:R12\n",   /* 너무 길다 */
    "A1-CH1:R1\n",    /* 구분자 오류 */
    "A1:XX1:R1\n",    /* CH 자리 오류 */
    "A1:CH1-R1\n",    /* 구분자 오류 */
    "A1:CH1:X1\n",    /* R 자리 오류 */
    "B1:CH1:R1\n",    /* 시작 글자 오류 */
    "garbage\n",
  };
  size_t i;

  for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i)
  {
    reset_parser();
    feed(bad[i]);
    CHECK(stat_ack_bad == 1U, "\"%s\" not rejected (bad=%u)", bad[i], stat_ack_bad);
    CHECK(stat_ack_ok == 0U, "\"%s\" wrongly accepted", bad[i]);
    CHECK(queued_count == 0U, "\"%s\" was queued", bad[i]);
  }
}

/** CR/LF 어느 쪽으로 끝나도 같게 동작하고, 빈 줄은 아무것도 세지 않아야 한다. */
static void test_line_endings(void)
{
  reset_parser();
  feed("A1:CH1:R1\r\n");          /* CRLF -- 사이의 빈 줄이 오류로 세이면 안 된다 */
  CHECK(stat_ack_ok == 1U, "CRLF: ok=%u", stat_ack_ok);
  CHECK(stat_ack_bad == 0U, "CRLF: empty line counted bad (%u)", stat_ack_bad);

  reset_parser();
  feed("\n\n\r\n");               /* 빈 줄만 */
  CHECK(stat_ack_ok == 0U && stat_ack_bad == 0U,
        "empty lines counted: ok=%u bad=%u", stat_ack_ok, stat_ack_bad);
}

/**
 * 버퍼를 넘긴 줄은 개행이 올 때까지 통째로 버려야 한다.
 * 잘라서 해석하면 뒷부분이 다음 줄의 앞머리처럼 보여 엉뚱한 ACK가 만들어진다.
 * 그리고 그 다음 줄은 정상으로 돌아와야 한다(영구히 망가지면 안 된다).
 */
static void test_overflow_discards_whole_line(void)
{
  reset_parser();
  feed("AAAAAAAAAAAAAAAAAAAAAAAAA1:CH1:R2\n");   /* 버퍼(16)를 훌쩍 넘긴다 */
  CHECK(stat_ack_ok == 0U, "overflowed line produced an ACK");
  CHECK(queued_count == 0U, "overflowed line was queued");

  /* 다음 줄은 정상 처리되어야 한다 */
  feed("A1:CH1:R2\n");
  CHECK(stat_ack_ok == 1U, "parser did not recover after overflow (ok=%u)", stat_ack_ok);
}

/**
 * 프레임이 조각나서 들어와도(ISR 은 한 바이트씩 받는다) 결과가 같아야 한다.
 * 실기에서 ACK는 명령 직후에 오므로 다른 트래픽과 섞여 조각날 수 있다.
 */
static void test_byte_by_byte_fragmentation(void)
{
  reset_parser();
  feed("A1:");
  feed("CH");
  feed("2");
  feed(":R");
  feed("1");
  feed("\n");

  CHECK(stat_ack_ok == 1U, "fragmented ACK not assembled (ok=%u)", stat_ack_ok);
  CHECK(last_ack.channel == 2U && last_ack.risk_level == 1U, "fragmented ACK parsed wrong");
}

/**
 * 실기에서 실제로 관찰된 흐름을 그대로 재생한다.
 * 명령 에코 -> 다른 슬레이브의 ACK -> 우리 ACK 가 섞여 들어온다.
 * ack_bad 가 0이어야 한다 -- 실기 [stat] 에서 ack_bad 가 부팅 조각 1건 말고는 늘지
 * 않았던 것이 이 성질이다.
 */
static void test_realistic_bus_traffic(void)
{
  reset_parser();
  feed("S1:CH1:R0\n");     /* 우리 명령의 에코 */
  feed("A1:CH1:R0\n");     /* Slave1 의 ACK */
  feed("S2:CH3:R2\n");     /* 우리 명령의 에코 */
  feed("A2:CH3:R2\n");     /* Slave2 의 ACK */
  feed("S1:CH2:R0\n");
  feed("A1:CH2:R0\n");

  CHECK(stat_ack_ok == 3U, "expected 3 ACKs, got %u", stat_ack_ok);
  CHECK(stat_ack_bad == 0U, "realistic traffic produced %u bad lines", stat_ack_bad);
  CHECK(queued_count == 3U, "expected 3 queued, got %u", queued_count);
}

int main(void)
{
  test_valid_ack();
  test_all_valid_combinations();
  test_own_command_echo_is_silent();
  test_other_channel_ack_is_accepted();
  test_malformed_lines_rejected();
  test_line_endings();
  test_overflow_discards_whole_line();
  test_byte_by_byte_fragmentation();
  test_realistic_bus_traffic();

  printf("ack_parse: all checks passed\n");
  return 0;
}
