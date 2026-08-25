/**
 * @file sched_dispatch.inc
 * @brief sched_task 가 한 틱에 하는 일 전부: 채널 하나를 골라(pick_pending) 회선에 내보내고
 *        (dispatch_channel) 그 결과를 채널 상태에 반영한다. main.c 가 그대로 포함한다.
 *
 * sched_select.inc 와 같은 이유로 별도 파일이다 -- 추상화가 아니라 검증이다.
 * sched_select.inc 는 "누구를 고르는가"만 담고, 여기는 "고른 뒤 상태가 어떻게 바뀌는가"를
 * 담는다. 예전에는 이 상태 전이(applied_risk / last_tx_tick / retry 갱신)를 호스트 테스트가
 * 손으로 흉내냈고(tools/test_sched_select.c 의 commit()/fail()), 그래서 실제 dispatch_channel 이
 * 다르게 갱신하면 "테스트는 통과하는데 실기는 틀린" 구멍이 있었다(TESTING.md 참고).
 * tools/test_sched_loop.c 가 이 파일을 그대로 포함해 실제 코드를 돌려 그 구멍을 메운다.
 *
 * 컴파일 단위가 아니다(.c 가 아니므로 CubeIDE 가 따로 빌드하지 않는다). 포함하는 쪽이
 * 아래를 먼저 갖추고 있어야 한다:
 *   - 자료:   channel_ctrl[] / channel_status[] / sched_cursor / channel_status_mutex
 *   - 상수:   CHANNEL_COUNT / SCHED_NO_CHANNEL / RS485_ACK_REQUIRED / VEDA_UPLINK_REASON_ACK
 *   - seam:   send_channel_command() / wait_for_slave_ack() / drain_ack_queue() /
 *             enqueue_uplink() / risk_to_status() / pick_pending()  (앞의 sched_select.inc)
 *   - 시각:   osKernelGetTickCount() / osMutexAcquire() / osMutexRelease() / osWaitForever
 *   - 카운터: stat_rs485_tx_fail / stat_sched_retry / stat_sched_refresh /
 *             stat_lat_max / stat_lat_danger_max
 *   - 필드:   channel_ctrl[].desired_tick / .lat_pending / .lat_risk (지연 계측용)
 *
 * 위 seam 여섯 개가 이 파일이 하드웨어/RTOS 와 닿는 유일한 경계다. 호스트 테스트는 그 여섯 개만
 * 가짜로 갈아끼우고(가짜 Slave 모델), 상태 전이 로직은 실기와 100% 같은 것을 돌린다.
 */

/**
 * @brief 채널 하나를 회선에 내보내고 그 결과를 상태에 반영한다. sched_task 전용.
 * @param channel_index 0-based 내부 채널 인덱스
 * @param risk_level 내보낼 등급. 호출자가 스냅샷한 값이어야 한다.
 * @param is_refresh 값이 그대로인데 주기가 되어 다시 보내는 경우 1
 * @details 실패하면 applied_risk 를 건드리지 않는다. 그러면 다음 틱에 desired != applied 가
 * 그대로 남아 같은 채널이 자연히 다시 잡힌다 -- 재시도 큐도 타이머도 따로 필요 없다.
 */
static void dispatch_channel(uint8_t channel_index, uint8_t risk_level, uint8_t is_refresh)
{
  channel_status_t next;
  uint8_t ok;

  /* 앞 명령의 늦은 ACK가 남아 있으면 이번 명령의 확인으로 오인된다. 보내기 전에 비운다. */
  drain_ack_queue();

  /* 위험 등급을 그대로 싣는다. 여기서 ON/OFF로 압축하면 Slave가 WARNING과 DANGER를
   * 영영 구분할 수 없다. */
  ok = send_channel_command(channel_index, risk_level);
  if (ok == 0U)
  {
    ++stat_rs485_tx_fail;
  }

#if RS485_ACK_REQUIRED
  /* 송신 성공은 '회선에 바이트를 밀어넣었다'까지만 보장한다. Slave가 실제로 GPIO를
   * 썼는지는 ACK로만 알 수 있다 -- 이게 없으면 Slave 전원이 꺼져 있거나 MY_SLAVE_ID가
   * 어긋나 프레임을 통째로 버려도 우리가 RPi에게 '정상 점등 중'이라고 보고하게 된다. */
  if (ok != 0U)
  {
    ok = wait_for_slave_ack(channel_index, risk_level);
  }
#endif

  channel_ctrl[channel_index].last_tx_tick = osKernelGetTickCount();

  if (ok != 0U)
  {
    channel_ctrl[channel_index].applied_risk = risk_level;
    channel_ctrl[channel_index].retry = 0U;

    /* 반응 지연 계측: 이 채널에 걸려 있던 '새 명령'이 방금 Slave까지 확인됐다면
     * 그 왕복 시간을 기록한다. 등급이 다르면(그 사이 RPi가 다시 바꾼 것) 이번 건이
     * 아니므로 pending 을 그대로 두어 다음 회차가 재게 한다.
     * 부호 없는 뺄셈이라 틱이 감겨도 정확하다. */
    if (channel_ctrl[channel_index].lat_pending != 0U &&
        channel_ctrl[channel_index].lat_risk == risk_level)
    {
      const uint32_t lat = osKernelGetTickCount() - channel_ctrl[channel_index].desired_tick;

      channel_ctrl[channel_index].lat_pending = 0U;
      if (lat > stat_lat_max)
      {
        stat_lat_max = lat;
      }
      if (risk_level == (uint8_t)VEDA_RISK_DANGER && lat > stat_lat_danger_max)
      {
        stat_lat_danger_max = lat;
      }
    }

    risk_to_status(risk_level, &next);
    (void)osMutexAcquire(channel_status_mutex, osWaitForever);
    channel_status[channel_index] = next;
    (void)osMutexRelease(channel_status_mutex);
  }
  else
  {
    /* applied_risk 를 그대로 둔다. 상행 ACK가 예전 상태를 싣고 올라가 RPi도 불일치를 알게
     * 되지만, 이제는 그쪽 재전송을 기다리지 않는다 -- 다음 틱에 우리가 직접 다시 보낸다.
     * retry 는 진단용이고, 재시도를 성립시키는 것은 이 값이 아니라 applied 를 안 건드린 것이다. */
    ++channel_ctrl[channel_index].retry;
    ++stat_sched_retry;
  }

  /* 주기 리프레시는 새로 알릴 것이 없다. 상태는 HEARTBEAT가 이미 500ms마다 싣고 올라간다. */
  if (is_refresh == 0U)
  {
    enqueue_uplink(channel_index, (uint8_t)VEDA_UPLINK_REASON_ACK);
  }
}

/**
 * @brief sched_task 의 한 틱: 처리할 채널을 골라 회선에 내보낸다.
 * @details StartSchedTask 의 루프에서 osDelayUntil 로 20ms 를 재운 직후 이 함수를 부른다.
 * 시간 재우기(osDelayUntil)만 루프에 남기고 선택+송신 로직 전부를 여기로 뺀 이유는,
 * 호스트 테스트가 실제 대기 없이 이 함수만 반복해 돌릴 수 있게 하기 위함이다.
 *
 * 우선순위는 DANGER 먼저다(1차 패스). 경보를 켜는 일이 끄는 일보다 항상 급하기 때문이다.
 * 두 패스가 같은 커서를 공유하므로 DANGER 채널이 여럿이어도 자기들끼리 라운드로빈이 돌고,
 * DANGER가 계속 들어와도 NONE/WARNING이 밀리는 것은 한 틱뿐이라 눈에 보이지 않는다.
 *
 * 고른 순간의 desired 를 지역 변수(want)로 스냅샷한다. 송신 도중 ctrl_task 가 desired 를
 * 바꿔도 이번 회차는 이 값으로 일관되게 끝나고, 다음 틱에 불일치가 다시 잡혀 새 값이 나간다.
 */
static void sched_tick(void)
{
  uint8_t ch;
  uint8_t want;
  uint8_t is_refresh;

  ch = pick_pending(1U);            /* 1차: DANGER 만 */
  if (ch == SCHED_NO_CHANNEL)
  {
    ch = pick_pending(0U);          /* 2차: 나머지를 라운드로빈 */
  }
  if (ch == SCHED_NO_CHANNEL)
  {
    return;                         /* 모든 채널이 맞춰져 있다 */
  }

  want = channel_ctrl[ch].desired_risk;
  is_refresh = (uint8_t)((want == channel_ctrl[ch].applied_risk) ? 1U : 0U);
  if (is_refresh != 0U)
  {
    ++stat_sched_refresh;
  }

  sched_cursor = (uint8_t)((ch + 1U) % CHANNEL_COUNT);
  dispatch_channel(ch, want, is_refresh);
}
