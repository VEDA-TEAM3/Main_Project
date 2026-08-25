/**
 * @file veda_config.h
 * @brief Master 의 모든 튜닝 상수와 규약 상수. **이 시스템의 설계 근거가 모여 있는 곳이다.**
 *
 * 여기 있는 값은 대부분 "왜 이 숫자인가"에 대한 답이 함께 있어야만 의미가 있다.
 * 근거 없이 값만 바꾸면 호스트 테스트(tools/)가 실패하거나, 더 나쁘게는 통과한 채로
 * 실기에서만 굶주림·복구 불능 같은 형태로 드러난다. 값을 고치기 전에 그 항목의 주석을
 * 끝까지 읽고, tools/test_sched_loop.c 를 반드시 다시 돌릴 것.
 *
 * 이 헤더는 자료도 함수도 갖지 않는다(순수 상수). 어느 모듈이 포함해도 부작용이 없다.
 */

#ifndef VEDA_CONFIG_H
#define VEDA_CONFIG_H

/* ===========================================================================
 * 명령 입력 경로
 * =========================================================================== */

/**
 * Master가 명령을 얻는 경로는 두 가지이고, 둘 중 하나만 켜야 한다.
 *  1) MASTER_SELFTEST_ENABLED = 0 (기본): 관제 서버(Raspberry Pi)가 USART6로 보낸
 *     veda_downlink_frame_t 를 받아 담당 Slave로 전달한다.
 *  2) MASTER_SELFTEST_ENABLED = 1: RPi 없이 순서표대로 위험 이벤트를 스스로 만들어 넣는다.
 *     배선 bring-up용. 상행(HEARTBEAT/ACK)은 이 모드에서도 그대로 나간다.
 *
 * 둘을 동시에 켜면 자동 순서표와 RPi 명령이 같은 채널을 서로 다른 상태로 밀어
 * 경광등이 멋대로 깜빡인다. 원인 찾기가 매우 어려우므로 전환해서 쓸 것.
 */
#define MASTER_SELFTEST_ENABLED 0

/** 이 간격으로 순서표의 다음 명령을 하나씩 넣는다. (MASTER_SELFTEST_ENABLED 전용) */
#define RS485_SEND_INTERVAL_MSEC 3000U

/* ===========================================================================
 * RS-485 하행 (Master -> Slave, USART1)
 * =========================================================================== */

/** Slave로 RS-485(USART1) 프레임을 보낼 때의 타임아웃 */
#define RS485_TX_TIMEOUT_MSEC 100U

/** 프레임 조립 버퍼 크기. "S1:CH1:R2\n"이 10자다. */
#define RS485_FRAME_BUFFER_SIZE 16U

/**
 * Slave의 ACK를 '적용 확인'으로 요구할지 여부. 이 값이 이 기능의 안전 스위치다.
 *
 * 0 (기본) = 진단 전용. ACK를 받아 [stat]의 ack_ok/ack_bad/ack_to에 집계만 하고, 채널
 *     상태는 예전처럼 송신 성공 시점에 갱신한다. 동작과 지연이 종전과 완전히 같으므로
 *     배선이 단방향이어도 굽는 즉시 나빠지는 것이 없다.
 * 1 = 실제 검증. ACK가 타임아웃 안에 오고 내용까지 일치할 때만 applied_risk 와
 *     channel_status[]를 갱신한다. ACK가 없으면 applied 가 그대로 남아 sched_task 가
 *     다음 틱에 같은 채널을 다시 잡는다 -- 즉 RS-485 구간의 유실을 Master 가 직접 복구한다.
 *
 * 스케줄러를 들이기 전에는 이 값을 1로 올릴 수 없었다. 재시도 주체가 없어서, ACK를 한 번
 * 놓치면 그 채널이 영구 불일치로 굳고 RPi의 재전송만 기다려야 했기 때문이다. sched_task 가
 * 생기면서 1이 정상 동작이 됐다.
 *
 * 그래도 순서는 지킬 것: 먼저 0으로 구워 [stat] ack_ok가 올라가는지 확인한다. 0에서 안
 * 올라가면 회선이 반이중으로 동작하지 않는 것이므로(트랜시버 DE/RE 고정 배선 등), 1로
 * 올리면 모든 채널이 매 틱 재전송에 들어가 버스가 그것만으로 찬다.
 */
#define RS485_ACK_REQUIRED 1

/**
 * Slave가 ACK를 되보낼 때까지 기다리는 시간. Slave 태스크가 10ms 주기로 폴링해 보내므로
 * 그 지연과 송신 시간(약 1ms)에 여유를 얹었다. (RS485_ACK_REQUIRED == 1 에서만 쓴다)
 */
#define RS485_ACK_TIMEOUT_MSEC 100U

/** ACK 줄 조립 버퍼. "A1:CH1:R2"가 9자다. */
#define RS485_ACK_LINE_BUFFER_SIZE 16U

/** ISR -> sched_task ACK 큐 깊이. 명령 하나당 한 장이면 충분하나 부팅 시 4연발을 흡수한다. */
#define ACK_QUEUE_DEPTH 8U

/**
 * RS-485 트랜시버의 송신 드라이버(DE/RE)를 GPIO로 제어할지 여부.
 * Slave 쪽 같은 이름 매크로와 반드시 함께 맞출 것 -- 한쪽만 켜면 회선이 한 방향으로 잠긴다.
 *
 * 0 = 방향 제어 없음(자동 방향 전환 트랜시버 또는 고정 배선).
 * 1 = MAX485 등 수동 트랜시버. DE와 RE를 묶어 RS485_DE_Pin 에 연결할 것.
 *
 * PA4를 고른 이유: 이 보드에서 쓰이지 않는 유일한 인접 핀이다(PA0 릴레이, PA2/PA3 USART2,
 * PA5 LD2, PA8 네오픽셀, PA9/PA10 USART1, PA13/PA14 SWD, PC6/PC7 USART6가 이미 점유).
 */
#define RS485_DE_ENABLED 1
#define RS485_DE_Pin GPIO_PIN_4
#define RS485_DE_GPIO_Port GPIOA

/* ===========================================================================
 * 스케줄러
 * =========================================================================== */

/**
 * 스케줄러 틱. 한 틱에 채널 하나만 처리하므로 이 값이 곧 RS-485 버스의 최소 명령 간격이다.
 *
 * 20ms를 고른 이유: 4채널을 도는 데 80ms라 사람 눈에 즉시로 보이면서, Slave의 태스크
 * 루프(osDelay(10))가 ACK를 되보낼 여유가 한 주기 이상 남는다. 이보다 짧게 잡으면
 * Slave의 단일 ACK 슬롯(ack_pending)이 덮어써져 ACK가 조용히 사라진다.
 */
#define SCHED_TICK_MSEC 20U

/**
 * 채널마다 이 시간이 지나면 값이 그대로여도 한 번 다시 보낸다.
 *
 * !! 이것이 없으면 안 된다. 예전에는 ctrl_task 가 같은 등급을 중복으로 재전송해서, RPi의
 *    checkChannelMismatch() 재전송이 그대로 회선까지 흘러 복구 경로 노릇을 했다. 스케줄러는
 *    desired == applied 인 채널을 건너뛰므로 그 경로가 죽는다. 대신 여기가 복구를 맡는다:
 *    Slave 가 리셋되거나 프레임을 놓쳐도 늦어도 이 시간 안에 상태가 다시 맞춰진다.
 *
 * !! Slave 에는 링크 두절 failsafe 가 없다(fail-loud -- 마지막 상태를 그대로 유지한다).
 *    그래서 이 주기 리프레시가 **어긋난 Slave 를 되돌리는 유일한 경로**다. Slave 가 리셋되든
 *    프레임을 놓치든 선이 잠깐 끊겼다 붙든, 상태를 다시 맞춰 주는 것은 여기뿐이다.
 *    이 값을 키우면 그만큼 어긋난 상태가 오래 남는다는 뜻이므로 신중할 것.
 *
 * 최악 조건(보드 한 대 사망 + 랜덤 부하)에서 살아 있는 채널이 방치되는 최대 시간은
 * tools/test_sched_loop.c 의 test_refresh_period_guarantee 가 재고 상한을 못박는다
 * (측정값 2128ms). SCHED_RETRY_BACKOFF_MSEC 를 잘못 줄이면 이 값이 먼저 터진다.
 */
#define SCHED_REFRESH_MSEC 2000U

/**
 * 송신이나 ACK가 실패한 채널을 다시 잡기까지 비워 두는 시간.
 *
 * !! 이것이 없으면 DANGER 채널 하나가 전체를 굶긴다. DANGER 우선 패스는 대기 중인 DANGER
 *    채널을 매 틱 무조건 돌려주므로, 그 채널의 ACK가 계속 실패하면 2차 패스가 영영 실행되지
 *    않는다 -- 나머지 채널의 주기 리프레시가 통째로 멈춘다. Slave 한 대가 죽었을 때 살아
 *    있는 다른 Slave까지 상태 동기화를 잃는 것이라, 정확히 막아야 할 고장 형태다.
 *
 * !! 값을 정할 때 ACK 타임아웃과 '같게' 두면 안 된다. 한때 100U(= RS485_ACK_TIMEOUT_MSEC)
 *    였는데, 그 근거는 "실패한 채널이 5틱을 쉬니 그 사이 다른 채널이 돈다"였다. 이 계산이
 *    틀렸다. 대기 시간은 last_tx_tick 부터 재는데 그 값은 ACK 대기가 '끝난' 뒤에 찍히고,
 *    실패한 다른 DANGER 채널도 각자 타임아웃만큼 회선을 붙잡는다. 그래서 실패한 DANGER
 *    채널이 둘이면 서로 번갈아 도는 사이에 각자의 대기가 저절로 지나가 버려서, 둘 다 매번
 *    자격을 되찾고 우선순위 패스가 영영 양보하지 않는다 -- 살아 있는 채널이 굶는다.
 *    (한 대가 죽고 그 보드의 두 채널이 모두 DANGER 인 상황이 정확히 이 경우다.)
 *
 *    조건: 실패 중인 DANGER 채널이 K개일 때, 한 바퀴 도는 데 (K-1) * (TX + ACK 타임아웃)
 *    ≈ (K-1) * 101ms 가 걸린다. 이 시간보다 대기가 짧으면 굶주림이 발생한다.
 *    K=3(한 채널만 살아남은 최악)이면 202ms 를 넘겨야 하므로 넉넉히 500ms 로 둔다.
 *
 * 500ms 로 올려도 잃는 것이 없다: 첫 시도는 이 대기를 받지 않으므로(retry == 0 이면
 * 건너뛴다) 새 명령은 여전히 즉시 나가고, 실패한 채널도 초당 두 번은 복구를 시도한다.
 * 주기 리프레시(2000ms)보다 여전히 4배 짧아 복구 경로도 그대로 살아 있다.
 *
 * 회귀 검사: tools/test_sched_loop.c 의 test_dead_board_isolation 이 이 값을 100 으로
 * 되돌리면 실패한다(실패한 ACK가 실제로 100ms 를 소비하도록 모델링돼 있다).
 */
#define SCHED_RETRY_BACKOFF_MSEC 500U

/** pick_pending() 이 "처리할 채널 없음"을 알리는 값 */
#define SCHED_NO_CHANNEL 0xFFU

/**
 * applied_risk 의 부팅값 -- "Slave 가 지금 무엇을 표시하고 있는지 아직 모른다".
 *
 * 0/1/2 어느 등급과도 다르므로 스케줄러가 첫 틱에 desired 와의 불일치로 잡아
 * 평소와 똑같은 경로(송신 -> ACK 확인 -> channel_status 갱신 -> 상행 ACK)로 내보낸다.
 * 회선에는 절대 나가지 않는다 -- dispatch_channel() 이 싣는 것은 언제나 desired 쪽 값이다.
 */
#define SCHED_RISK_UNKNOWN 0xFFU

/* ===========================================================================
 * 채널 배치 (RPi / Slave 와 함께 맞춰야 하는 값)
 * =========================================================================== */

/**
 * 전체 Channel 개수. RPi의 config.json channelCount(=4) 와 같아야 한다.
 * 카메라 1~4번이 각각 Channel 1~4에 대응한다.
 */
#define CHANNEL_COUNT 4U

/**
 * Slave 한 대가 담당하는 Channel 수. Master/Slave 양쪽의 라우팅 규칙이 이 값 하나로 정해진다.
 * 바꾸려면 Rs485_receive_demo 의 같은 이름 매크로도 함께 바꿔야 한다.
 *
 *   CH1, CH2 -> Slave #1        CH3, CH4 -> Slave #2
 *
 * Slave 는 담당 채널마다 별도의 출력을 구동한다(첫 채널 PA0, 두 번째 PA1).
 * 보드당 채널 수를 늘리려면 이 값과 Slave 의 channel_output[] 표를 함께 늘리면 된다.
 */
#define CHANNELS_PER_SLAVE 2U

/**
 * RPi가 보내는 veda_risk_event_t.channel_id 의 시작 번호.
 *
 * !! 이 값이 RPi 쪽과 어긋나면 엉뚱한 경광등이 켜지거나 모든 프레임이 버려진다.
 *    한 번은 반드시 실물로 확인하고 넘어갈 것. 판별은 부팅 배너와 USART2 로그의
 *    bad_ch 카운터로 한다(범위 밖 channel_id 가 오면 그 값이 그대로 찍힌다).
 *
 *  - control-server(운영 경로)  : zoneId == channelId 이고 zoneId 는 0부터  -> 0
 *  - veda_frame_send.cpp(수동)  : kAllowedChannels = {1, 3}                 -> 1
 *
 * 내부 channel index 는 항상 0-based 이고, RS-485 프레임의 슬레이브/채널 숫자는
 * 항상 1-based 다(Slave의 process_rs485_line()이 '0'을 형식 오류로 버린다).
 * 즉 이 매크로는 "RPi가 쓰는 번호 체계"만 흡수한다.
 */
#define RPI_CHANNEL_ID_BASE 0U

/* ===========================================================================
 * 상행 / 하행 (Master <-> RPi, USART6)
 * =========================================================================== */

/** 관제 서버(USART6)로 상행 프레임을 보낼 때의 타임아웃 */
#define UPLINK_TX_TIMEOUT_MSEC 100U

/**
 * HEARTBEAT 송신 주기.
 * RPi의 config.json hwHealthCheck.heartbeatIntervalMs 와 같아야 한다. RPi는
 * heartbeatIntervalMs * missedBeatsForTimeout(=3) 동안 소식이 없으면 그 채널을
 * dead 로 판정하므로, 이 값을 키우려면 저쪽도 같이 키워야 한다.
 */
#define HEARTBEAT_INTERVAL_MSEC 500U

/**
 * 하행 프레임을 조립하는 중 이 시간 동안 바이트가 하나도 없으면 조각을 버리고 재동기화한다.
 * 없으면 전송이 중간에 끊긴 프레임의 잔해가 다음 프레임 앞에 붙어 계속 체크섬을 깨뜨린다.
 */
#define DOWNLINK_RESYNC_MSEC 100U

/** USART6 ISR -> rx_task 로 넘기는 원시 바이트 큐 깊이 (하행 한 프레임이 27바이트) */
#define RPI_RX_QUEUE_DEPTH 128U

/** rx_task -> ctrl_task 위험 이벤트 큐 깊이 */
#define CMD_QUEUE_DEPTH 8U

/** ctrl_task/hb_task -> tx_task 상행 패킷 큐 깊이. 한 주기 분량 + ACK 여유 */
#define UPLINK_QUEUE_DEPTH ((CHANNEL_COUNT * 2U) + 4U)

/* ===========================================================================
 * 진단 (USART2 = ST-Link VCP)
 * =========================================================================== */

/**
 * USART2(ST-Link VCP, 115200)로 진단 로그를 낸다. 0으로 두면 코드가 남지 않는다.
 *
 * 이 로그가 필요한 이유: 하행이 바이너리로 바뀌면서 USART6 는 사람이 읽을 수 없게 됐고,
 * 프레임이 버려질 때 Master 는 밖에서 볼 수 있는 흔적을 전혀 남기지 않는다.
 * "프레임이 안 온다 / 왔는데 체크섬이 깨졌다 / channel_id 가 범위 밖이다 / RS-485 송신이
 * 실패했다"를 구분하려면 별도 회선이 필요하다.
 */
#define MASTER_DEBUG_LOG 1

/** 감시 태스크가 카운터를 찍는 주기 */
#define SUPERVISOR_LOG_INTERVAL_MSEC 5000U

/** 감시 태스크가 깨어나는 주기 (LD2 점멸 분해능) */
#define SUPERVISOR_TICK_MSEC 100U

/** 이 시간 동안 유효한 하행 프레임이 없으면 링크가 조용한 것으로 보고 LD2를 느리게 점멸한다 */
#define LINK_IDLE_WARN_MSEC 5000U

/** LD2 점멸 주기: 하행이 살아있을 때 / 조용할 때 */
#define LD2_BLINK_ALIVE_MSEC 500U
#define LD2_BLINK_IDLE_MSEC 2000U

#endif /* VEDA_CONFIG_H */
