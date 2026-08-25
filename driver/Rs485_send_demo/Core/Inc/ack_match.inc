/**
 * @file ack_match.inc
 * @brief 방금 보낸 명령에 대한 ACK 를 골라내는 규칙. main.c 가 그대로 포함한다.
 *
 * ack_parse.inc 가 "이 줄이 형식상 ACK 인가"를 보는 곳이라면, 여기는 그 다음 질문을 본다:
 * **"큐에 들어온 이 ACK 가 내가 방금 보낸 명령에 대한 답이 맞는가?"**
 *
 * 이 판별이 왜 까다로운가:
 *  - RS-485 는 버스를 공유하므로 옆 채널로 간 ACK 도 큐에 들어온다.
 *  - 앞 명령의 ACK 가 늦게 도착해 큐에 남아 있을 수 있다. 그걸 이번 명령의 확인으로
 *    오인하면 **적용되지 않은 명령을 적용됐다고 기록**하게 된다 -- 이 시스템에서 가장
 *    위험한 거짓말이다.
 *  - 그래서 두 겹으로 막는다: 보내기 직전에 큐를 비우고(drain), 받은 뒤에는 채널과 등급을
 *    둘 다 대조한다. 어느 한쪽만으로는 부족하다.
 *
 * 컴파일 단위가 아니다(.c 가 아니므로 CubeIDE 가 따로 빌드하지 않는다). 포함하는 쪽이
 * 아래를 먼저 갖추고 있어야 한다:
 *   - 자료:   rs485_ack_t
 *   - 상수:   RS485_ACK_TIMEOUT_MSEC
 *   - 카운터: stat_ack_timeout / stat_ack_mismatch
 *   - seam:   ack_queue_take(rs485_ack_t *out, uint32_t timeout_ms) -> 1 받음 / 0 없음
 *             ack_now_ms(void) -> 현재 시각(ms)
 *
 * 실기에서는 seam 이 CMSIS-RTOS 큐와 osKernelGetTickCount 로 이어지고,
 * 호스트 테스트(tools/test_ack_match.c)에서는 대본대로 ACK 를 흘려주는 가짜로 이어진다.
 */

/**
 * @brief 큐에 남아 있는 ACK 를 모두 버린다.
 * @details 앞 명령에 대한 늦은 ACK 가 남아 있으면 이번 명령의 확인으로 오인된다.
 * 명령을 보내기 직전에 불러 그 창을 닫는다. 집계는 ISR 의 ack_feed_line() 이 이미 했으므로
 * 여기서는 세지 않는다.
 */
static void drain_ack_queue(void)
{
  rs485_ack_t discarded;

  while (ack_queue_take(&discarded, 0U) != 0U)
  {
    /* 버리기만 한다. */
  }
}

#if RS485_ACK_REQUIRED
/**
 * @brief 방금 보낸 명령에 대한 Slave 의 ACK 를 기다린다.
 * @param channel_index 0-based 내부 채널 인덱스
 * @param risk_level 방금 보낸 veda_risk_level_t
 * @retval 1 내용까지 일치하는 ACK 를 받았다, 0 타임아웃이다
 * @details 회선 위 채널 번호는 1-based 라 여기서 맞춰 비교한다. 다른 채널이나 다른 등급의
 * ACK 가 오면 앞 명령의 늦은 응답이거나 잡음이므로, 버리고 **남은 시간만큼 계속 기다린다**
 * -- 여기서 바로 실패로 처리하면 옆 채널의 ACK 하나 때문에 멀쩡한 명령이 재전송된다.
 *
 * 등급까지 비교하므로 WARNING 을 보냈는데 DANGER 가 적용된 경우도 불일치로 잡힌다.
 *
 * 경과 시간을 부호 없는 뺄셈으로 재므로 시각이 감겨도(49.7일) 정확하다.
 */
static uint8_t wait_for_slave_ack(uint8_t channel_index, uint8_t risk_level)
{
  const uint8_t expect_channel = (uint8_t)(channel_index + 1U);
  const uint32_t start = ack_now_ms();
  rs485_ack_t ack;

  for (;;)
  {
    const uint32_t elapsed = ack_now_ms() - start;
    uint32_t remaining;

    if (elapsed >= RS485_ACK_TIMEOUT_MSEC)
    {
      ++stat_ack_timeout;
      return 0U;
    }
    remaining = RS485_ACK_TIMEOUT_MSEC - elapsed;

    if (ack_queue_take(&ack, remaining) == 0U)
    {
      ++stat_ack_timeout;
      return 0U;
    }

    if (ack.channel == expect_channel && ack.risk_level == risk_level)
    {
      return 1U;
    }

    /* 내 것이 아니다. 남은 시간 동안 계속 기다린다. */
    ++stat_ack_mismatch;
  }
}
#endif /* RS485_ACK_REQUIRED */
