/**
 * @file ack_parse.inc
 * @brief Slave가 되보낸 ACK 줄을 조립하고 해석하는 규칙. main.c 가 그대로 포함한다.
 *
 * sched_select.inc / sched_dispatch.inc 와 같은 이유로 별도 파일이다 -- 추상화가 아니라
 * 검증이다. 이 코드는 ISR 문맥에서 돌아 실기에서는 관찰이 거의 불가능한데(로그를 찍으면
 * 그 자체로 오버런이 난다), 하는 일은 순수한 문자열 판별이라 호스트에서 전부 검사할 수 있다.
 * tools/test_ack_parse.c 가 이 파일을 포함해 검사한다.
 *
 * 컴파일 단위가 아니다(.c 가 아니므로 CubeIDE 가 따로 빌드하지 않는다). 포함하는 쪽이
 * 아래를 먼저 갖추고 있어야 한다:
 *   - 자료:   rs485_ack_t / ack_line[] / ack_line_length / ack_line_overflow
 *   - 상수:   RS485_ACK_LINE_BUFFER_SIZE
 *   - 카운터: stat_ack_ok / stat_ack_bad
 *   - seam:   ack_queue_put(const rs485_ack_t *)  -- 해석된 ACK 한 장을 넘길 곳
 */

/**
 * @brief RS-485 ACK 한 줄을 해석해 큐로 넘긴다.
 * @param line 개행을 제외한 줄 본문 (NUL로 끝나지 않음)
 * @param length line의 길이
 * @details Slave가 보내는 "A<슬레이브>:CH<채널>:R<위험도>" 만 받아들인다.
 * 첫 글자로 우리가 내보낸 명령('S')과 구분되므로, 트랜시버가 자기 송신을 되울리는
 * 구성이어도 자기 명령을 ACK로 오해하지 않는다.
 *
 * 위험도 자리는 0/1/2 만 받는다. 그 밖의 값은 우리가 아는 규약이 아니므로 ack_bad 로
 * 세고 버린다 -- 임의로 해석하면 Slave가 무엇을 적용했는지 잘못 기록하게 된다.
 */
static void ack_feed_line(const char *line, uint8_t length)
{
  rs485_ack_t ack;

  /* 우리가 방금 내보낸 명령이 되울려 들어온 것이다(자동 방향 전환 트랜시버는 송신 중에도
   * 로컬 수신기가 열려 있다). 정상이므로 ack_bad로 세지 않고 조용히 버린다 -- 여기서 세면
   * 명령 하나마다 ack_bad가 올라가 진짜 형식 오류를 가려 버린다. */
  if (length != 0U && line[0] == 'S')
  {
    return;
  }

  /* 고정 위치 검사: "A1:CH1:R2" (항상 9자) */
  if (length != 9U || line[0] != 'A' || line[2] != ':' ||
      line[3] != 'C' || line[4] != 'H' || line[6] != ':' || line[7] != 'R')
  {
    ++stat_ack_bad;
    return;
  }
  if (line[1] < '1' || line[1] > '9' || line[5] < '1' || line[5] > '9')
  {
    ++stat_ack_bad;
    return;
  }
  if (line[8] < '0' || line[8] > '2')
  {
    ++stat_ack_bad;
    return;
  }

  ack.risk_level = (uint8_t)(line[8] - '0');
  ack.slave = (uint8_t)(line[1] - '0');
  ack.channel = (uint8_t)(line[5] - '0');
  ++stat_ack_ok;

  /* 큐가 가득 차면 버린다. RS485_ACK_REQUIRED가 0이면 sched_task가 큐를 비우지 않으므로
   * 정상적으로 가득 차며, 그때도 위의 stat_ack_ok 집계는 계속 올라간다. */
  ack_queue_put(&ack);
}

/**
 * @brief RS-485로 받은 바이트 하나를 ACK 줄로 조립한다. ISR 문맥에서 불린다.
 * @details 조립만 하고 해석은 개행에서 한 번에 한다. ACK는 최대 11바이트라 이 정도
 * 처리는 한 바이트 시간(115200 기준 약 87us) 안에 끝난다.
 *
 * 버퍼를 넘긴 줄은 개행이 올 때까지 통째로 버린다. 잘라서 해석하면 뒷부분이 다음 줄의
 * 앞머리처럼 보여 엉뚱한 ACK가 만들어질 수 있다.
 */
static void ack_feed_byte(uint8_t byte)
{
  if (byte == '\r' || byte == '\n')
  {
    if (ack_line_overflow == 0U && ack_line_length != 0U)
    {
      ack_feed_line(ack_line, ack_line_length);
    }
    ack_line_length = 0U;
    ack_line_overflow = 0U;
    return;
  }

  if (ack_line_length >= (uint8_t)RS485_ACK_LINE_BUFFER_SIZE)
  {
    ack_line_overflow = 1U;
    return;
  }

  ack_line[ack_line_length] = (char)byte;
  ++ack_line_length;
}
