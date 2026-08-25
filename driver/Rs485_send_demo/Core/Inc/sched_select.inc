/**
 * @file sched_select.inc
 * @brief sched_task 가 "이번 틱에 어느 채널을 보낼지" 고르는 규칙. main.c 가 그대로 포함한다.
 *
 * 별도 파일로 둔 이유는 추상화가 아니라 검증이다. 이 규칙은 우선순위/라운드로빈/주기
 * 리프레시가 한 루프 안에서 엮여 있어 눈으로 읽어서는 굶는 채널이나 커서 역행을 잡기
 * 어렵다. 호스트에서 돌리는 tools/test_sched_select.c 가 같은 파일을 포함해 검사한다.
 *
 * 컴파일 단위가 아니다(.c 가 아니므로 CubeIDE 가 따로 빌드하지 않는다). 포함하는 쪽이
 * channel_ctrl / sched_cursor / CHANNEL_COUNT / SCHED_REFRESH_MSEC /
 * SCHED_RETRY_BACKOFF_MSEC / SCHED_NO_CHANNEL / VEDA_RISK_DANGER /
 * osKernelGetTickCount() 를 먼저 갖추고 있어야 한다.
 */

/**
 * @brief 이번 틱에 처리할 채널 하나를 커서 위치부터 찾는다.
 * @param danger_only 1이면 DANGER로 올라가는 채널만 고른다
 * @retval SCHED_NO_CHANNEL 처리할 것이 없다
 * @details 커서에서 시작해 한 바퀴만 돈다. 매번 0부터 훑으면 CH1에 이벤트가 계속 들어올 때
 * CH4가 영영 밀린다.
 *
 * 경과 시간을 부호 없는 뺄셈으로 재므로 osKernelGetTickCount()가 감겨도 정확하다.
 */
static uint8_t pick_pending(uint8_t danger_only)
{
  uint8_t i;

  for (i = 0U; i < CHANNEL_COUNT; ++i)
  {
    const uint8_t ch = (uint8_t)((sched_cursor + i) % CHANNEL_COUNT);
    const uint8_t want = channel_ctrl[ch].desired_risk;
    const uint32_t idle = osKernelGetTickCount() - channel_ctrl[ch].last_tx_tick;

    if (want != channel_ctrl[ch].applied_risk)
    {
      /* 방금 실패한 채널은 잠시 양보시킨다. 이게 없으면 응답하지 않는 DANGER 채널 하나가
       * 우선순위 패스를 독점한다 -- 대기 중인 DANGER를 매 틱 무조건 돌려주므로 2차 패스가
       * 영영 실행되지 않고, 살아 있는 다른 채널의 리프레시까지 멈춘다. Slave 한 대가
       * 죽었을 때 나머지 Slave와의 상태 동기화까지 잃는 것이라 정확히 막아야 할 고장이다.
       *
       * 첫 시도(retry == 0)는 대기 없이 즉시 나간다 -- 새 명령을 늦추면 안 된다. */
      if (channel_ctrl[ch].retry != 0U && idle < SCHED_RETRY_BACKOFF_MSEC)
      {
        continue;
      }

      if (danger_only == 0U || want == (uint8_t)VEDA_RISK_DANGER)
      {
        return ch;
      }
      continue;
    }

    /* 값은 맞지만 오래됐다. Slave가 리셋되거나 프레임을 놓쳤어도 여기서 다시 맞춰진다.
     * 리프레시는 급하지 않으므로 DANGER 우선 패스에서는 건너뛴다 -- 여기서 고르면
     * 이미 맞춰진 채널이 진짜 DANGER 전환보다 앞서게 된다. */
    if (danger_only == 0U && idle >= SCHED_REFRESH_MSEC)
    {
      return ch;
    }
  }

  return SCHED_NO_CHANNEL;
}
