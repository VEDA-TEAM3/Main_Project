/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

/* RPi(control-server)와 공유하는 UART 통신 규약.
 * 원본은 RPi의 Main_Project_MQTT/shared/driver_protocol.h 이고 이 파일은 그 사본이다.
 * 체크섬/구조체 정의가 갈라지면 프레임이 조용히 어긋나므로, 규약이 바뀌면 반드시
 * 원본을 다시 복사해 올 것(손으로 고치지 말 것). */
#include "driver_protocol.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
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

/** Slave가 ACK를 되보낼 때까지 기다리는 시간. Slave 태스크가 10ms 주기로 폴링해 보내므로
 *  그 지연과 송신 시간(약 1ms)에 여유를 얹었다. (RS485_ACK_REQUIRED == 1 에서만 쓴다) */
#define RS485_ACK_TIMEOUT_MSEC 100U
/** ACK 줄 조립 버퍼. "A1:CH1:R2"가 9자다. */
#define RS485_ACK_LINE_BUFFER_SIZE 16U
/** ISR -> sched_task ACK 큐 깊이. 명령 하나당 한 장이면 충분하나 부팅 시 4연발을 흡수한다. */
#define ACK_QUEUE_DEPTH 8U

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
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE BEGIN PV */
/** 관제 서버(Raspberry Pi)와 연결된 UART. .ioc에 없으므로 아래에서 직접 초기화한다. */
UART_HandleTypeDef huart6;

/* 규약이 갈라지거나 컴파일러가 몰래 패딩을 넣으면 RPi 파서와 어긋난다.
 * RPi 쪽(veda_frame_send.cpp, SerialHwEventDispatcher)과 같은 크기를 빌드 시점에 못박는다. */
_Static_assert(sizeof(veda_risk_event_t) == 24, "veda_risk_event_t must be 24 bytes");
_Static_assert(sizeof(veda_uplink_packet_t) == 16, "veda_uplink_packet_t must be 16 bytes");
_Static_assert(sizeof(veda_downlink_frame_t) == 27, "veda_downlink_frame_t must be 27 bytes");
_Static_assert(sizeof(veda_uplink_frame_t) == 19, "veda_uplink_frame_t must be 19 bytes");

/**
 * @brief Channel 하나가 지금 표시하고 있는 상태. 상행 ACK/HEARTBEAT payload의 원본이다.
 * @details 이 구조체는 "Master가 Slave에게 지령해서 실제로 나간 값"만 담는다. RS-485는
 * 단방향이라 Slave가 확인 응답을 주지 않으므로, 송신에 실패하면 여기를 갱신하지 않는다.
 * 그러면 다음 ACK가 예전 상태를 싣고 올라가고 RPi의 checkChannelMismatch()가 불일치를
 * 잡아 재전송한다 -- 실패를 조용히 삼키는 대신 상대의 재시도 경로를 타게 하는 것이다.
 */
typedef struct
{
  uint8_t risk_level;   /**< 마지막으로 적용한 veda_risk_level_t */
  uint8_t siren_on;     /**< PA0 경광등 지령 상태. 이 시스템의 유일한 액추에이터다. */
  uint8_t buzzer_on;    /**< 부저는 배선된 적이 없다. 항상 0을 올린다. */
  uint8_t led_red;      /**< 아래 3개는 물리 LED가 아니라 '표시 중인 위험 등급'의 인코딩이다. */
  uint8_t led_yellow;   /**< RPi의 decodeRiskLevel()이 red->Danger, yellow->Warning, */
  uint8_t led_green;    /**< 그 외->None 으로 되읽으므로 이 규약을 그대로 지켜야 한다. */
} channel_status_t;

/** Channel별 표시 상태. 인덱스는 0-based(= channel_id - RPI_CHANNEL_ID_BASE). */
static channel_status_t channel_status[CHANNEL_COUNT];
/** channel_status 를 sched_task(쓰기)와 hb_task(읽기)가 나눠 쓰므로 보호한다. */
static osMutexId_t channel_status_mutex;
static const osMutexAttr_t channel_status_mutex_attributes = {
  .name = "channelStatusMutex",
};

/**
 * @brief Channel 하나의 스케줄링 상태. "무엇을 원하는가"와 "무엇이 실제로 나갔는가"를 가른다.
 * @details 이 분리가 스케줄러의 전부다. ctrl_task 는 desired 만 적고 끝내고, sched_task 가
 * desired != applied 인 채널을 찾아 회선에 내보낸다.
 *
 * 이 구조가 고치는 실제 증상: 예전에는 ctrl_task 가 이벤트 하나당 프레임 하나를 그대로
 * 밀어서, RPi가 DANGER를 연달아 보내면 그게 전부 버스에 쌓였다. 쌓인 것이 뒤늦게 몰아서
 * 재생되니 경광등이 서너 번 깜빡이다 갑자기 꺼지고, RPi가 아는 상태와 어긋났다.
 * 여기서는 같은 등급이 몇 번 오든 desired 한 칸을 덮어쓸 뿐이라 백로그 자체가 생기지 않는다.
 *
 * desired_risk 에 뮤텍스를 두지 않는 이유: 1바이트이고 쓰는 쪽이 ctrl_task 하나, 읽는 쪽이
 * sched_task 하나뿐이다. Cortex-M 의 바이트 접근은 원자적이라 찢어진 값을 읽을 수 없다.
 * sched_task 는 고른 순간의 값을 지역 변수로 스냅샷해 그 회차 내내 그것만 쓰므로, 송신 도중
 * desired 가 바뀌어도 다음 틱에 desired != applied 로 다시 잡혀 새 값이 나간다.
 */
typedef struct
{
  volatile uint8_t desired_risk; /**< RPi가 마지막으로 지시한 등급. ctrl_task 만 쓴다. */
  uint8_t          applied_risk; /**< 실제로 Slave까지 확인된 등급. sched_task 만 쓴다. */
  uint32_t         last_tx_tick; /**< 마지막 송신 시각. 주기 리프레시 기준이다. */
  uint16_t         retry;        /**< 연속 실패 횟수. 진단용이다. */

  /* --- 반응 지연 계측 (진단 전용. 스케줄링 판단에는 쓰지 않는다) ---------------
   * "RPi의 명령이 Slave까지 확인되는 데 걸린 시간"을 재기 위한 것이다. 눈으로는
   * 20ms 와 80ms 를 구분할 수 없어서, 경보 장비의 반응 상한을 숫자로 말하려면
   * 이 계측이 필요하다. ctrl_task 가 찍고 sched_task 가 읽어 갱신한다.
   * 워드 단위 접근이라 Cortex-M 에서 찢어진 값이 읽히지 않는다. */
  uint32_t         desired_tick; /**< desired 가 '새 값으로 바뀐' 시각 */
  uint8_t          lat_pending;  /**< 그 변경이 아직 Slave까지 확인되지 않았다 */
  uint8_t          lat_risk;     /**< 그 변경이 요구한 등급 (DANGER 만 따로 집계) */
} channel_ctrl_t;

/** Channel별 스케줄링 상태. 인덱스는 channel_status[] 와 같다. */
static channel_ctrl_t channel_ctrl[CHANNEL_COUNT];

/** 라운드로빈 커서. 매번 0부터 훑으면 앞 채널이 계속 밀릴 때 뒤 채널이 굶는다. */
static uint8_t sched_cursor;

/** pick_pending() 이 "처리할 채널 없음"을 알리는 값 */
#define SCHED_NO_CHANNEL 0xFFU

/** USART6 ISR -> rx_task 원시 바이트 큐 */
static osMessageQueueId_t rpi_rx_queue;
/** rx_task -> ctrl_task 검증이 끝난 위험 이벤트 큐 */
static osMessageQueueId_t cmd_queue;
/** ctrl_task/hb_task -> tx_task 상행 패킷 큐 */
static osMessageQueueId_t uplink_queue;

/** Slave가 되보낸 ACK 한 장. "A<슬레이브>:CH<채널>:R<위험도>" 를 파싱한 결과다. */
typedef struct
{
  uint8_t slave;      /**< 1-based 슬레이브 번호 */
  uint8_t channel;    /**< 1-based 채널 번호 (RS-485 회선 위 표현) */
  uint8_t risk_level; /**< Slave가 실제로 적용한 veda_risk_level_t (0/1/2) */
} rs485_ack_t;

/** USART1 ISR -> ctrl_task ACK 큐 */
static osMessageQueueId_t ack_queue;

/** USART6(관제 서버) 인터럽트 수신용 1바이트 버퍼 */
static uint8_t rpi_rx_byte;

/** USART1(RS-485) 인터럽트 수신용 1바이트 버퍼 */
static uint8_t rs485_rx_byte;
/** 조립 중인 ACK 줄. ISR 전용이라 별도 보호가 필요 없다. */
static char ack_line[RS485_ACK_LINE_BUFFER_SIZE];
static uint8_t ack_line_length;
/** 버퍼를 넘긴 줄은 개행이 올 때까지 통째로 버린다는 표시 */
static uint8_t ack_line_overflow;

/** 하행 프레임 동기화 상태머신. RPi의 SerialHwEventDispatcher::readerLoop()와 대칭이다. */
typedef enum
{
  DL_WAIT_START = 0,
  DL_READ_PAYLOAD,
  DL_READ_CHECKSUM,
  DL_WAIT_END
} downlink_state_t;

static downlink_state_t downlink_state;
static uint8_t downlink_payload[sizeof(veda_risk_event_t)];
static uint8_t downlink_payload_index;
static uint8_t downlink_rx_checksum;

/** osKernelGetTickCount()는 32비트라 약 49.7일에 한 번 감긴다. 그 접힘을 세어 64비트로 편다. */
static uint32_t ms_clock_last_tick;
static uint32_t ms_clock_wrap_count;

/* 진단 카운터. 감시 태스크가 USART2로 주기 출력한다. */
static volatile uint32_t stat_rx_bytes_dropped;  /**< 바이트 큐가 가득 차 ISR이 버린 수 */
static volatile uint32_t stat_frames_ok;         /**< 체크섬까지 통과한 하행 프레임 수 */
static volatile uint32_t stat_frames_bad;        /**< 체크섬/END 불일치 또는 중간에 끊긴 프레임 수 */
static volatile uint32_t stat_bad_channel;       /**< channel_id가 범위 밖이라 버린 이벤트 수 */
static volatile uint32_t stat_last_bad_channel;  /**< 마지막으로 거부한 channel_id (base 오설정 진단용) */
static volatile uint32_t stat_bad_risk;          /**< risk_level이 0/1/2가 아니라 버린 이벤트 수 */
static volatile uint32_t stat_last_bad_risk;     /**< 마지막으로 거부한 risk_level 값 */
static volatile uint32_t stat_cmd_dropped;       /**< ctrl_task가 밀려 버린 이벤트 수 */
static volatile uint32_t stat_rs485_tx_fail;     /**< RS-485 송신 실패 수 */
static volatile uint32_t stat_uplink_tx_fail;    /**< 상행 송신 실패 수 */
static volatile uint32_t stat_uplink_dropped;    /**< 상행 큐가 가득 차 버린 패킷 수 */
static volatile uint32_t stat_uplink_sent;       /**< 상행으로 내보낸 프레임 수 */
static volatile uint32_t stat_ack_ok;            /**< 형식이 맞는 ACK를 Slave에게서 받은 수 */
static volatile uint32_t stat_ack_bad;           /**< 형식이 어긋나 버린 ACK 줄 수 */
static volatile uint32_t stat_ack_timeout;       /**< 기다렸는데 ACK가 오지 않은 명령 수 */
static volatile uint32_t stat_ack_mismatch;      /**< ACK는 왔지만 보낸 명령과 내용이 다른 수 */
/** desired 와 같은 등급이 다시 와서 회선에 내보내지 않은 이벤트 수.
 *  예전 구조에서 그대로 버스에 나가 백로그를 만들던 것이 이 값이다. 크게 올라간다면
 *  RPi가 같은 등급을 반복 송신 중이라는 뜻이고, 그것이 정상이다(스케줄러가 흡수한다). */
static volatile uint32_t stat_cmd_coalesced;
/** sched_task 가 재전송한 횟수(송신 실패 또는 ACK 타임아웃 뒤 다시 시도한 수) */
static volatile uint32_t stat_sched_retry;
/**
 * 명령이 Slave까지 확인되기까지 걸린 시간의 최댓값(ms). 부팅 이후 누적이다.
 *
 * lat_dgr 이 이 시스템의 반응 상한이다 -- DANGER 는 경보를 켜는 명령이라 이 값만이
 * "경광등이 최악 몇 ms 안에 켜지는가"에 답한다. 설계상 예상치는 다음과 같다:
 *   정상    : 한 틱(20ms) + 채널 순번 대기. 4채널이므로 최악 80ms 근처.
 *   고장 중 : 앞 채널이 ACK 타임아웃(100ms)을 먹으면 그만큼 밀린다.
 * lat_max 는 등급을 가리지 않은 전체 최댓값이라 둘을 비교하면 DANGER 우선 패스가
 * 실제로 먼저 나가고 있는지 확인된다(lat_dgr 이 lat_max 보다 작아야 정상이다).
 */
static volatile uint32_t stat_lat_max;
static volatile uint32_t stat_lat_danger_max;
/** 값이 그대로인데 주기가 되어 다시 내보낸 수 (SCHED_REFRESH_MSEC) */
static volatile uint32_t stat_sched_refresh;
/** 마지막으로 유효한 하행 프레임을 받은 시각. 0이면 부팅 후 한 번도 받지 못한 것이다. */
static volatile uint32_t last_downlink_tick;

/* 태스크 핸들과 속성. 스택은 바이트 단위다(CMSIS-RTOS2 규약). */
static osThreadId_t rxTaskHandle;
static osThreadId_t ctrlTaskHandle;
static osThreadId_t schedTaskHandle;
static osThreadId_t txTaskHandle;
static osThreadId_t heartbeatTaskHandle;

/** 바이트 큐를 가장 먼저 비워야 오버런이 나지 않으므로 우선순위가 제일 높다. */
static const osThreadAttr_t rxTask_attributes = {
  .name = "rxTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
static const osThreadAttr_t ctrlTask_attributes = {
  .name = "ctrlTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/** 유일한 RS-485 송신자. ctrl_task 와 같은 우선순위로 둔다 -- 접수와 실행 중 어느 쪽도
 *  상대를 굶기지 않아야 하고, 둘 다 rx_task 보다는 뒤다(바이트 큐가 먼저 비어야 한다). */
static const osThreadAttr_t schedTask_attributes = {
  .name = "schedTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
static const osThreadAttr_t txTask_attributes = {
  .name = "txTask",
  .stack_size = 192 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/** 주기 보고라 밀려도 되지만, 밀린 만큼 RPi의 dead 판정에 가까워진다. */
static const osThreadAttr_t heartbeatTask_attributes = {
  .name = "hbTask",
  .stack_size = 192 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};

#if MASTER_SELFTEST_ENABLED
/** 흐름 테스트 순서표의 한 단계 */
typedef struct
{
  uint8_t channel_index;  /**< 0-based 내부 채널 인덱스 */
  uint8_t risk_level;     /**< veda_risk_level_t */
} test_step_t;

/**
 * 4개 채널을 순서대로 하나씩 켜고 끈다. 주기마다 한 단계씩 진행하고 끝에서 처음으로 돌아간다.
 *
 * 전 채널을 도는 이유: channel_index 는 슬레이브 번호가 아니라 채널 번호다.
 * CHANNELS_PER_SLAVE=2 이므로 index 0,1 은 둘 다 Slave #1 로 가고 index 2,3 이 Slave #2 로
 * 간다. 앞의 두 개만 넣어두면 Slave #2 로 구운 보드는 순서표 전체를 OTHER_SLAVE 로 버려서
 * "테스트를 돌렸는데 아무 일도 안 일어난다"가 된다. 어느 번호로 구운 보드가 꽂혀 있어도
 * 반드시 한 번은 반응하도록 4채널을 모두 돈다.
 *
 *   index 0 -> S1:CH1     index 1 -> S1:CH2
 *   index 2 -> S2:CH3     index 3 -> S2:CH4
 *
 * 채널마다 WARNING -> DANGER -> NONE 을 한 주기(3초)씩 보낸다. 어느 채널에서 불이 들어오는지
 * 눈으로 셀 수 있고, 동시에 Slave 줄 조명의 노랑/빨강/초록이 모두 나오는지 확인된다 --
 * 회선 규약이 등급을 그대로 싣게 바뀌었으므로 WARNING 단계가 DANGER 와 다른 색을 내야 한다.
 * 한 바퀴는 12단계 = 36초다.
 */
static const test_step_t test_sequence[] = {
  { 0U, (uint8_t)VEDA_RISK_WARNING },
  { 0U, (uint8_t)VEDA_RISK_DANGER  },
  { 0U, (uint8_t)VEDA_RISK_NONE    },
  { 1U, (uint8_t)VEDA_RISK_WARNING },
  { 1U, (uint8_t)VEDA_RISK_DANGER  },
  { 1U, (uint8_t)VEDA_RISK_NONE    },
  { 2U, (uint8_t)VEDA_RISK_WARNING },
  { 2U, (uint8_t)VEDA_RISK_DANGER  },
  { 2U, (uint8_t)VEDA_RISK_NONE    },
  { 3U, (uint8_t)VEDA_RISK_WARNING },
  { 3U, (uint8_t)VEDA_RISK_DANGER  },
  { 3U, (uint8_t)VEDA_RISK_NONE    },
};
#define TEST_SEQUENCE_LENGTH (sizeof(test_sequence) / sizeof(test_sequence[0]))

/** 다음에 보낼 순서표 위치 */
static uint8_t test_step_index;
#endif /* MASTER_SELFTEST_ENABLED */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */
static void StartRxTask(void *argument);
static void StartCtrlTask(void *argument);
static void StartSchedTask(void *argument);
static void StartTxTask(void *argument);
static void StartHeartbeatTask(void *argument);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/**
 * @brief 관제 서버(Raspberry Pi)와 연결된 USART6를 초기화한다.
 * @details USART6는 .ioc에 없어서 CubeMX가 코드를 만들어 주지 않으므로
 * 클럭·GPIO·NVIC까지 이 함수가 직접 처리한다. 전부 USER CODE 영역에 있어
 * CubeMX로 코드를 다시 생성해도 지워지지 않는다.
 *
 * 배선: PC6(USART6_TX) -> RPi RXD(GPIO15) / PC7(USART6_RX) <- RPi TXD(GPIO14), GND 공통
 * 다른 핀을 쓰려면 PA11(TX)/PA12(RX)도 같은 AF8로 USART6에 연결된다.
 */
static void rpi_uart_init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_USART6_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  GPIO_InitStruct.Pin = GPIO_PIN_6|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF8_USART6;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  huart6.Instance = USART6;
  huart6.Init.BaudRate = 115200;
  huart6.Init.WordLength = UART_WORDLENGTH_8B;
  huart6.Init.StopBits = UART_STOPBITS_1;
  huart6.Init.Parity = UART_PARITY_NONE;
  huart6.Init.Mode = UART_MODE_TX_RX;
  huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart6.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart6) != HAL_OK)
  {
    Error_Handler();
  }

  /* 우선순위 5는 FreeRTOS의 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY와 같은 값이다. */
  HAL_NVIC_SetPriority(USART6_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(USART6_IRQn);
}

/* ===========================================================================
 * 진단 로그 (USART2 = ST-Link VCP)
 * =========================================================================== */
#if MASTER_DEBUG_LOG
/**
 * @brief USART2(ST-Link VCP)로 문자열을 내보낸다. 감시 태스크 문맥에서만 부를 것.
 * @details 블로킹 송신이라 ISR이나 실시간 경로에서 부르면 하행 수신이 밀려 오버런이 난다.
 * huart2를 쓰는 곳을 감시 태스크 하나로 제한해 송신 경합도 함께 없앤다.
 */
static void dbg_print(const char *text)
{
  (void)HAL_UART_Transmit(&huart2, (const uint8_t*)text, (uint16_t)strlen(text), 100U);
}

/**
 * @brief 부호 없는 10진수를 찍는다.
 * @details printf 계열을 쓰지 않는 이유는 스택과 코드 크기다. 감시 태스크 스택을
 * 512바이트로 유지하려면 가변 인자 포맷터를 들일 수 없다.
 */
static void dbg_print_u32(uint32_t value)
{
  char reversed[10];
  char text[10];
  uint8_t count = 0U;
  uint8_t index;

  do
  {
    reversed[count] = (char)('0' + (uint8_t)(value % 10U));
    ++count;
    value /= 10U;
  } while (value != 0U);

  for (index = 0U; index < count; ++index)
  {
    text[index] = reversed[count - 1U - index];
  }

  (void)HAL_UART_Transmit(&huart2, (const uint8_t*)text, count, 100U);
}

/** @brief "<label>=<value> " 한 토막을 찍는다. */
static void dbg_print_stat(const char *label, uint32_t value)
{
  dbg_print(label);
  dbg_print("=");
  dbg_print_u32(value);
  dbg_print(" ");
}

/**
 * @brief 태스크별로 '한 번이라도 남았던 스택의 최솟값'을 바이트로 찍는다.
 * @details uxTaskGetStackHighWaterMark() 는 그 태스크가 살아온 동안 스택이 가장 적게
 * 남았던 순간의 여유를 워드 단위로 돌려준다. 즉 이 값이 0에 가까우면 이미 한 번은
 * 아슬아슬했다는 뜻이다 -- 스택 넘침은 평소엔 멀쩡하다가 특정 경로에서만 터지는 고장이라
 * 잘 도는 지금 여유를 재 두는 것이 요점이다.
 *
 * 판단 기준: 여유가 100바이트 아래로 내려간 태스크가 있으면 그 태스크의 stack_size 를
 * 키운다. 특히 dbg_print 계열을 부르는 defaultTask 와 ACK 대기가 있는 schedTask 를 볼 것.
 *
 * 계측 대상이 아니라 진단이므로 감시 태스크에서만 부른다(huart2 송신자를 하나로 유지).
 */
static void dbg_print_stack_free(const char *label, osThreadId_t handle)
{
  if (handle == NULL)
  {
    return;
  }
  /* CMSIS-RTOS2 의 osThreadId_t 는 FreeRTOS TaskHandle_t 와 같은 포인터다. */
  dbg_print_stat(label, (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)handle) * 4U);
}

/** @brief "[stack] ..." 한 줄. 태스크마다 남은 스택 바이트를 찍는다. */
static void dbg_print_stack_line(void)
{
  dbg_print("[stack] ");
  dbg_print_stack_free("dflt", defaultTaskHandle);
  dbg_print_stack_free("rx", rxTaskHandle);
  dbg_print_stack_free("ctrl", ctrlTaskHandle);
  dbg_print_stack_free("sched", schedTaskHandle);
  dbg_print_stack_free("tx", txTaskHandle);
  dbg_print_stack_free("hb", heartbeatTaskHandle);
  dbg_print("(bytes free)\r\n");
}
#else
#define dbg_print(text)             ((void)0)
#define dbg_print_u32(value)        ((void)0)
#define dbg_print_stat(label, val)  ((void)0)
#define dbg_print_stack_line()      ((void)0)
#endif /* MASTER_DEBUG_LOG */

/* ===========================================================================
 * 시각
 * =========================================================================== */
/**
 * @brief 부팅 후 경과 밀리초를 64비트로 돌려준다. 상행 payload의 timestamp_ms 값이다.
 * @details STM32에는 RTC 배터리가 없으므로 이 값은 벽시계(Unix epoch)가 아니라 부팅 기준이다.
 * RPi는 상행 timestamp를 dead 판정이나 불일치 판정에 쓰지 않고(수신 시각을 자체적으로 찍는다)
 * 로그 상관용으로만 쓰므로 이 정의로 충분하다. 벽시계가 필요해지면 RPi가 하행 프레임에 실어
 * 보내는 timestamp_ms로 오프셋을 잡는 것이 다음 단계다.
 *
 * osKernelGetTickCount()는 32비트라 약 49.7일에 감긴다. 접힘을 세어 시간이 뒤로 가지 않게 한다.
 * 여러 태스크가 부르므로 검사와 갱신을 임계구역으로 묶는다.
 */
static int64_t veda_now_ms(void)
{
  uint32_t now;
  uint32_t wraps;

  taskENTER_CRITICAL();
  now = osKernelGetTickCount();   /* configTICK_RATE_HZ == 1000 이므로 1틱 = 1ms */
  if (now < ms_clock_last_tick)
  {
    ++ms_clock_wrap_count;
  }
  ms_clock_last_tick = now;
  wraps = ms_clock_wrap_count;
  taskEXIT_CRITICAL();

  return (int64_t)(((uint64_t)wraps << 32) | (uint64_t)now);
}

/* ===========================================================================
 * 채널 상태
 * =========================================================================== */
/**
 * @brief 위험 등급을 이 채널이 표시할 상태로 변환한다. 하행 -> 상행 의미 변환의 유일한 지점이다.
 * @details led_* 는 물리 LED가 아니라 RPi가 되읽을 위험 등급의 인코딩이다
 * (decodeRiskLevel(): red->Danger, yellow->Warning, 그 외->None).
 * 여기 매핑을 바꾸면 RPi의 명령-상태 불일치 판정이 그대로 틀어지므로 저쪽과 함께 볼 것.
 *
 * WARNING도 경광등을 상시 점등한다(규격의 경광등/부저 정책). 등급 자체는 여기서 뭉개지
 * 않는다 -- out->risk_level 이 그대로 남고, 그 값이 RS-485 프레임에 실려 Slave까지 간다.
 * Slave는 그것으로 줄 조명 색(초록/노랑/빨강)을 가른다.
 */
static void risk_to_status(uint8_t risk_level, channel_status_t *out)
{
  out->risk_level = risk_level;

  switch (risk_level)
  {
    case (uint8_t)VEDA_RISK_DANGER:
      out->siren_on = 1U;
      out->led_red = 1U;
      out->led_yellow = 0U;
      out->led_green = 0U;
      break;

    case (uint8_t)VEDA_RISK_WARNING:
      out->siren_on = 1U;
      out->led_red = 0U;
      out->led_yellow = 1U;
      out->led_green = 0U;
      break;

    case (uint8_t)VEDA_RISK_NONE:
    default:
      /* 규약에 없는 등급은 ctrl_task 가 이 함수에 들어오기 전에 거절하므로 default 로는
       * 오지 않는다. 그래도 남겨 둔다 -- 다른 호출자가 생겨도 경광등을 켠 채로 두지 않게
       * 안전한 쪽(소등)으로 떨어뜨린다. */
      out->risk_level = (uint8_t)VEDA_RISK_NONE;
      out->siren_on = 0U;
      out->led_red = 0U;
      out->led_yellow = 0U;
      out->led_green = 1U;
      break;
  }

  /* Slave의 부저는 DANGER 에서만 운다(Slave의 apply_channel_state() 참고). 경광등과 조건이
   * 다르므로 siren_on을 따라가면 안 된다 -- 그러면 WARNING 구간에서 RPi의
   * HwIndicatorState(buzzerOn)가 실물과 어긋나 조용한 부저를 울고 있다고 보고하게 된다.
   *
   * 두 곳이 같은 규칙을 따라야 한다. 정책을 바꾸려면 이 줄과 Slave의 apply_channel_state()를
   * 반드시 함께 고칠 것:
   *
   *   등급       줄 조명   경광등   부저
   *   --------   -------   ------   ----
   *   NONE       초록      OFF      OFF
   *   WARNING    노랑      ON       OFF
   *   DANGER     빨강      ON       ON
   */
  out->buzzer_on = (uint8_t)((out->risk_level == (uint8_t)VEDA_RISK_DANGER) ? 1U : 0U);
}

/** @brief 부팅 직후 모든 채널을 '위험 없음'으로 초기화한다. 스케줄러 시작 전에 부를 것. */
static void channel_status_init(void)
{
  uint8_t index;

  for (index = 0U; index < CHANNEL_COUNT; ++index)
  {
    risk_to_status((uint8_t)VEDA_RISK_NONE, &channel_status[index]);
  }
}

/* ===========================================================================
 * RS-485 하행 (Master -> Slave)
 * =========================================================================== */
/**
 * @brief RS-485 트랜시버의 송신 드라이버를 제어할 수 있게 DE 핀을 출력으로 잡는다.
 * @details RS485_DE_ENABLED가 0이면 아무것도 하지 않는다(자동 방향 전환 트랜시버).
 * 기본 상태는 수신(DE=Low)이다 -- 명령을 보내는 순간에만 버스를 잡아야 Slave의 ACK가
 * 회선에 실릴 수 있다.
 */
static void rs485_de_init(void)
{
#if RS485_DE_ENABLED
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();

  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = RS485_DE_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(RS485_DE_GPIO_Port, &GPIO_InitStruct);
#endif
}

/**
 * @brief RS-485 버스(USART1, PA9/PA10)로 "S<슬레이브>:CH<채널>:R<위험도>\n" 프레임을 보낸다.
 * @param channel_index 0-based 내부 채널 인덱스
 * @param risk_level 보낼 veda_risk_level_t (0 = NONE, 1 = WARNING, 2 = DANGER)
 * @retval 1 송신 성공, 0 실패
 * @details 위험 등급을 ON/OFF로 압축하지 않고 숫자 그대로 싣는다. 예전 규약("...:ON"/
 * "...:OFF")에서는 WARNING과 DANGER가 회선 위에서 같은 ON 하나로 합쳐져, Slave가 두 상태를
 * 영영 구분할 수 없었다 -- 줄 조명의 노랑이 나올 수 없던 이유가 이것이다.
 *
 * 새 규약은 항상 9자 고정이라 Slave의 고정 위치 검사가 그대로 값 범위 검사를 겸한다.
 * 옛 펌웨어가 섞여 있으면 서로의 프레임을 형식 오류로 버린다(조용히 오해하지 않는다).
 *
 * 버스를 두 Slave가 공유하므로 프레임 앞의 슬레이브 번호가 주소 역할을 한다.
 * 자기 번호가 아닌 프레임은 Slave가 무시한다.
 *
 * 회선 위의 두 숫자는 모두 1-based 다. Slave의 process_rs485_line()이 '0' 자리를 형식 오류로
 * 버리므로 0-based 숫자를 그대로 실을 수 없다. RPi가 0-based를 쓰든 1-based를 쓰든
 * (RPI_CHANNEL_ID_BASE) RS-485 회선 위의 표현은 이 함수에서 하나로 고정된다.
 *
 *   index 0 -> "S1:CH1"   index 1 -> "S1:CH2"
 *   index 2 -> "S2:CH3"   index 3 -> "S2:CH4"
 *
 * 슬레이브 번호와 채널 번호는 같지 않다. 보드당 1채널이던 시절에는 두 숫자가 같아서 하나로
 * 계산했는데, 그 코드를 그대로 두면 CH3이 S3(존재하지 않는 보드)으로 나가 조용히 사라진다.
 *
 * sched_task에서만 부른다 -- huart1 송신자를 하나로 묶어 HAL의 gState 경합을 없앤다.
 * (예전에는 ctrl_task 가 소유했다. 스케줄러를 들이면서 송신 소유권이 통째로 옮겨왔다.)
 */
static uint8_t send_channel_command(uint8_t channel_index, uint8_t risk_level)
{
  char frame[RS485_FRAME_BUFFER_SIZE];
  uint8_t length = 0U;
  HAL_StatusTypeDef status;
  const char slave_digit = (char)('1' + (channel_index / CHANNELS_PER_SLAVE));
  const char channel_digit = (char)('1' + channel_index);

  frame[length++] = 'S';
  frame[length++] = slave_digit;
  frame[length++] = ':';
  frame[length++] = 'C';
  frame[length++] = 'H';
  frame[length++] = channel_digit;
  frame[length++] = ':';
  frame[length++] = 'R';
  frame[length++] = (char)('0' + (risk_level % 10U));
  frame[length++] = '\n';

#if RS485_DE_ENABLED
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_SET);
#endif

  status = HAL_UART_Transmit(&huart1, (const uint8_t*)frame, length, RS485_TX_TIMEOUT_MSEC);

#if RS485_DE_ENABLED
  /* HAL_UART_Transmit()은 마지막 바이트의 TC(전송 완료)까지 기다린 뒤 반환하므로
   * 여기서 바로 내려도 마지막 비트가 잘리지 않는다. 내려야 Slave가 ACK를 실을 수 있다. */
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
#endif

  return (status == HAL_OK) ? 1U : 0U;
}

/** @brief 해석이 끝난 ACK 한 장을 sched_task 로 넘긴다. ack_parse.inc 의 seam 이다. */
static void ack_queue_put(const rs485_ack_t *ack)
{
  (void)osMessageQueuePut(ack_queue, ack, 0U, 0U);
}

/* ACK 줄 조립·해석 규칙. 호스트 테스트(tools/test_ack_parse.c)가 같은 파일을 포함한다 --
 * ISR 문맥이라 실기에서는 관찰이 거의 불가능하므로 규칙을 고치면 저기 검사도 같이 볼 것. */
#include "ack_parse.inc"

/* ===========================================================================
 * 상행 (Master -> RPi, USART6)
 * =========================================================================== */
/**
 * @brief 지금의 채널 상태로 상행 패킷을 만들어 송신 큐에 넣는다.
 * @param channel_index 0-based 내부 채널 인덱스
 * @param reason veda_uplink_reason_t (ACK 또는 HEARTBEAT)
 * @details 큐가 가득 차면 버린다. 상행은 주기 보고라서 한 장 놓쳐도 다음 장이 곧 올라오고,
 * 여기서 블로킹하면 ctrl_task가 하행 처리를 멈추게 된다 -- 경광등 반응 지연이 더 나쁘다.
 */
static void enqueue_uplink(uint8_t channel_index, uint8_t reason)
{
  veda_uplink_packet_t packet;
  channel_status_t status;

  (void)memset(&packet, 0, sizeof(packet));   /* reserved0 = 0 -- 규약 */

  (void)osMutexAcquire(channel_status_mutex, osWaitForever);
  status = channel_status[channel_index];
  (void)osMutexRelease(channel_status_mutex);

  packet.channel_id = (uint8_t)(channel_index + RPI_CHANNEL_ID_BASE);
  packet.reason = reason;
  packet.siren_on = status.siren_on;
  packet.buzzer_on = status.buzzer_on;
  packet.led_red = status.led_red;
  packet.led_yellow = status.led_yellow;
  packet.led_green = status.led_green;
  packet.timestamp_ms = veda_now_ms();

  if (osMessageQueuePut(uplink_queue, &packet, 0U, 0U) != osOK)
  {
    ++stat_uplink_dropped;
  }
}

/**
 * @brief 상행 패킷을 veda_uplink_frame_t(19B)로 감싸 USART6로 내보낸다.
 * @details tx_task에서만 부른다. USART6는 수신이 인터럽트, 송신이 블로킹이고 HAL이
 * gState/RxState를 따로 관리하므로 수신과 겹쳐도 되지만, 송신자가 둘이 되면 프레임이
 * 바이트 단위로 섞여 RPi가 어느 쪽도 재조립하지 못한다. 그래서 송신자를 하나로 묶는다.
 */
static void uplink_send(const veda_uplink_packet_t *packet)
{
  veda_uplink_frame_t frame;

  frame.start_byte = VEDA_START_BYTE;
  frame.payload = *packet;
  frame.checksum = veda_uplink_checksum(&frame.payload);
  frame.end_byte = VEDA_END_BYTE;

  if (HAL_UART_Transmit(&huart6, (const uint8_t*)&frame, (uint16_t)sizeof(frame),
                        UPLINK_TX_TIMEOUT_MSEC) == HAL_OK)
  {
    ++stat_uplink_sent;
  }
  else
  {
    ++stat_uplink_tx_fail;
  }
}

/* ===========================================================================
 * 하행 (RPi -> Master, USART6)
 * =========================================================================== */
/**
 * @brief 하행 바이트 하나를 상태머신에 넣고, 프레임이 완성되면 payload를 꺼낸다.
 * @param byte 수신한 바이트
 * @param out 완성된 프레임의 payload를 받을 곳 (반환값이 1일 때만 유효)
 * @retval 1 유효한 프레임을 하나 완성했다, 0 아직이다
 * @details RPi의 SerialHwEventDispatcher::readerLoop()와 대칭인 상태머신이다.
 * START를 찾을 때까지 앞의 쓰레기 바이트는 건너뛰고, payload(24B) + checksum + END가
 * 모두 맞아야 유효 프레임으로 본다. 어긋나면 조용히 버리고 START 탐색으로 돌아간다.
 *
 * payload 안에도 START_BYTE('S')와 같은 값이 나올 수 있다. 그래서 START만으로는
 * 프레임 경계를 확정하지 못하고, 체크섬과 END까지 맞아야 통과시킨다 -- 어긋난 경우
 * 다음 START부터 다시 시도하면서 자연히 재동기화된다.
 */
static uint8_t downlink_feed_byte(uint8_t byte, veda_risk_event_t *out)
{
  uint8_t accepted = 0U;

  switch (downlink_state)
  {
    case DL_WAIT_START:
      if (byte == VEDA_START_BYTE)
      {
        downlink_payload_index = 0U;
        downlink_state = DL_READ_PAYLOAD;
      }
      break;

    case DL_READ_PAYLOAD:
      downlink_payload[downlink_payload_index] = byte;
      ++downlink_payload_index;
      if (downlink_payload_index == (uint8_t)sizeof(downlink_payload))
      {
        downlink_state = DL_READ_CHECKSUM;
      }
      break;

    case DL_READ_CHECKSUM:
      downlink_rx_checksum = byte;
      downlink_state = DL_WAIT_END;
      break;

    case DL_WAIT_END:
    default:
      downlink_state = DL_WAIT_START;
      if ((byte == VEDA_END_BYTE) &&
          (veda_checksum(downlink_payload, sizeof(downlink_payload)) == downlink_rx_checksum))
      {
        (void)memcpy(out, downlink_payload, sizeof(*out));
        accepted = 1U;
      }
      else
      {
        ++stat_frames_bad;
      }
      break;
  }

  return accepted;
}

/**
 * @brief USART6(관제 서버) 수신 완료 인터럽트. 바이트를 큐에 넣고 즉시 재무장한다.
 * @details 프레임 조립도 ISR에서 하지 않는다. STM32F4 USART에는 RX FIFO가 없어 ISR이
 * 한 바이트 시간(115200 8N1 기준 약 87us)보다 오래 걸리면 곧바로 오버런이 난다.
 * 체크섬 계산(24바이트 XOR)과 큐 전달까지 ISR에 넣으면 그 예산을 갉아먹으므로,
 * 여기서는 큐에 넣는 것만 하고 상태머신은 rx_task가 돌린다.
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART6)
  {
    if (osMessageQueuePut(rpi_rx_queue, &rpi_rx_byte, 0U, 0U) != osOK)
    {
      /* 큐가 가득 찼다 = rx_task가 밀렸다. 버린 바이트는 프레임 하나를 깨뜨리고
       * 다음 프레임에서 재동기화된다. 카운터로 남겨 로그에서 보이게 한다. */
      ++stat_rx_bytes_dropped;
    }

    (void)HAL_UART_Receive_IT(huart, &rpi_rx_byte, 1U);
  }
  else if (huart->Instance == USART1)
  {
    /* Slave의 ACK. 최대 11바이트짜리 줄이라 조립까지 ISR에서 끝내도 한 바이트 시간을
     * 넘기지 않는다. 하행(USART6)처럼 태스크로 넘기지 않는 이유는, ACK를 기다리는
     * ctrl_task가 바로 이 결과를 큐에서 꺼내야 하기 때문이다. */
    ack_feed_byte(rs485_rx_byte);

    (void)HAL_UART_Receive_IT(huart, &rs485_rx_byte, 1U);
  }
}

/**
 * @brief USART6 오류(오버런/프레이밍 등) 처리. 조립 중이던 프레임을 버리고 재무장한다.
 * @details 재무장하지 않으면 오류 한 번으로 수신이 영구히 멈춘다. STM32F4 USART에는
 * RX FIFO가 없어 바이트 하나만 밀려도 오버런(ORE)이 나고, 그때 HAL이 RX 인터럽트를 끈다.
 *
 * 상태머신을 여기서 직접 되돌리지 않는 이유: downlink_state는 rx_task 소유라서 ISR이
 * 건드리면 경합이 된다. 오류로 바이트가 빠지면 체크섬이 깨져 어차피 버려지고,
 * 회선이 조용해지면 rx_task의 DOWNLINK_RESYNC_MSEC 타임아웃이 조각을 정리한다.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART6)
  {
    (void)HAL_UART_Receive_IT(huart, &rpi_rx_byte, 1U);
  }
  else if (huart->Instance == USART1)
  {
    /* 조립 중이던 ACK 조각을 버린다. ACK 줄은 ISR 소유라 여기서 직접 되돌려도 경합이 없다
     * (하행 상태머신을 건드리지 않는 것과 다른 점이다). */
    ack_line_length = 0U;
    ack_line_overflow = 0U;

    (void)HAL_UART_Receive_IT(huart, &rs485_rx_byte, 1U);
  }
}

/* ===========================================================================
 * 태스크
 * ---------------------------------------------------------------------------
 * 하행 한 장이 경광등까지 가는 경로를 단계마다 큐로 끊어 놓았다.
 *
 *   USART6 ISR --(바이트)--> rx_task --(veda_risk_event_t)--> ctrl_task --> RS-485
 *                                                                 |
 *                              hb_task(500ms 주기) --------+------+
 *                                                          v
 *                                              tx_task --> USART6 상행
 *
 * 이렇게 나눈 이유:
 *  - ISR을 짧게 유지해야 오버런이 나지 않는다(위 RxCpltCallback 주석 참고).
 *  - RS-485 송신(블로킹, 약 1ms)이 도는 동안에도 하행 수신은 계속 흘러야 한다.
 *  - HEARTBEAT는 하행 처리 상태와 무관하게 일정 주기로 나가야 한다. 같은 태스크에 두면
 *    하행이 몰릴 때 주기가 밀리고, 그대로 RPi의 dead 오판으로 이어진다.
 *  - USART6 송신자를 tx_task 하나로 묶어야 ACK와 HEARTBEAT가 섞이지 않는다.
 * =========================================================================== */

/**
 * @brief 하행 바이트를 프레임으로 조립해 검증된 위험 이벤트만 ctrl_task로 넘긴다.
 */
static void StartRxTask(void *argument)
{
  uint8_t byte;
  veda_risk_event_t event;

  (void)argument;

  /* 수신 시작. 이후 재무장은 각 콜백이 담당한다.
   * 스케줄러가 뜬 뒤에 여는 이유: 미리 열면 큐가 아직 없는 상태에서 바이트가 도착한다. */
  (void)HAL_UART_Receive_IT(&huart6, &rpi_rx_byte, 1U);

  for (;;)
  {
    if (osMessageQueueGet(rpi_rx_queue, &byte, NULL, DOWNLINK_RESYNC_MSEC) == osOK)
    {
      if (downlink_feed_byte(byte, &event) != 0U)
      {
        ++stat_frames_ok;
        last_downlink_tick = osKernelGetTickCount();

        if (osMessageQueuePut(cmd_queue, &event, 0U, 0U) != osOK)
        {
          ++stat_cmd_dropped;
        }
      }
    }
    else if (downlink_state != DL_WAIT_START)
    {
      /* 프레임 중간에서 회선이 조용해졌다 -> 남은 조각을 버린다. 그대로 두면 다음 프레임
       * 앞에 붙어 계속 체크섬을 깨뜨린다. */
      downlink_state = DL_WAIT_START;
      ++stat_frames_bad;
    }
    else
    {
      /* 조용하고 조립 중인 것도 없다 -- 정상 대기 */
    }
  }
}

/**
 * @brief ACK 큐에 남은 것을 모두 버린다.
 * @details 앞 명령에 대한 늦은 ACK가 남아 있으면 이번 명령의 확인으로 오인된다.
 * 명령을 보내기 직전에 불러 그 창을 닫는다.
 */
static void drain_ack_queue(void)
{
  rs485_ack_t discarded;

  while (osMessageQueueGet(ack_queue, &discarded, NULL, 0U) == osOK)
  {
    /* 버리기만 한다. 집계는 ISR의 ack_feed_line()이 이미 했다. */
  }
}

#if RS485_ACK_REQUIRED
/**
 * @brief 방금 보낸 명령에 대한 Slave의 ACK를 기다린다.
 * @param channel_index 0-based 내부 채널 인덱스
 * @param risk_level 방금 보낸 veda_risk_level_t
 * @retval 1 내용까지 일치하는 ACK를 받았다, 0 타임아웃이다
 * @details 회선 위 채널 번호는 1-based라 여기서 맞춰 비교한다. 다른 채널이나 다른 등급의
 * ACK가 오면 앞 명령의 늦은 응답이거나 잡음이므로, 버리고 남은 시간만큼 계속 기다린다.
 *
 * 등급까지 비교하므로 WARNING을 보냈는데 DANGER가 적용된 경우도 불일치로 잡힌다.
 *
 * 경과 시간을 부호 없는 뺄셈으로 재므로 osKernelGetTickCount()가 감겨도 정확하다.
 */
static uint8_t wait_for_slave_ack(uint8_t channel_index, uint8_t risk_level)
{
  const uint8_t expect_channel = (uint8_t)(channel_index + 1U);
  const uint32_t start = osKernelGetTickCount();
  rs485_ack_t ack;

  for (;;)
  {
    const uint32_t elapsed = osKernelGetTickCount() - start;
    uint32_t remaining;

    if (elapsed >= RS485_ACK_TIMEOUT_MSEC)
    {
      ++stat_ack_timeout;
      return 0U;
    }
    remaining = RS485_ACK_TIMEOUT_MSEC - elapsed;

    if (osMessageQueueGet(ack_queue, &ack, NULL, remaining) != osOK)
    {
      ++stat_ack_timeout;
      return 0U;
    }

    if (ack.channel == expect_channel && ack.risk_level == risk_level)
    {
      return 1U;
    }

    ++stat_ack_mismatch;
  }
}
#endif /* RS485_ACK_REQUIRED */

/**
 * @brief 위험 이벤트를 접수해 채널의 목표 등급(desired)만 적는다.
 * @details 회선에 언제 어떤 순서로 나갈지는 sched_task 가 정한다. 여기서 송신도 ACK 대기도
 * 하지 않으므로 이 태스크는 블로킹되지 않는다 -- 4채널 이벤트가 한꺼번에 몰려도 cmd_queue 가
 * 밀리지 않는다는 뜻이고, 그것이 접수와 실행을 가른 이유다.
 */
static void StartCtrlTask(void *argument)
{
  veda_risk_event_t event;
  channel_status_t next;
  uint32_t channel_offset;
  uint8_t channel_index;

  (void)argument;

  for (;;)
  {
    if (osMessageQueueGet(cmd_queue, &event, NULL, osWaitForever) != osOK)
    {
      continue;
    }

    /* base보다 작은 channel_id 는 32비트 뺄셈에서 감겨 아주 큰 값이 되므로, 아래 한 번의
     * 범위 검사가 '너무 작다'와 '너무 크다'를 함께 잡는다. base를 0으로 둔 빌드에서
     * "< base" 비교가 항상 거짓이라 경고가 뜨는 것도 이 형태면 피할 수 있다. */
    channel_offset = (uint32_t)event.channel_id - (uint32_t)RPI_CHANNEL_ID_BASE;
    if (channel_offset >= CHANNEL_COUNT)
    {
      /* 배선되지 않은 채널이거나 RPI_CHANNEL_ID_BASE 설정이 저쪽과 어긋난 것이다.
       * 마지막 값을 남겨 두면 로그만 보고 둘 중 어느 쪽인지 판별할 수 있다. */
      ++stat_bad_channel;
      stat_last_bad_channel = event.channel_id;
      continue;
    }
    channel_index = (uint8_t)channel_offset;

    /* 규약에 있는 등급은 0/1/2 뿐이다. 그 밖의 값(3, 10, 255 ...)은 회선이 깨졌거나 저쪽
     * 규약이 우리보다 앞선 것이다. 세어서 로그에 남기되, 이벤트를 버리지는 않는다.
     *
     * 한때 여기서 continue 로 버렸는데 그게 위험했다. 버리면 Master 가 Slave 에게 아무것도
     * 보내지 않아 경광등이 직전 상태 그대로 굳는다 -- 잘못된 값 하나 때문에 이미 울리고 있던
     * 경보를 끌 수도, 꺼야 할 것을 끌 수도 없게 된다. 아래 risk_to_status() 가 모르는 값을
     * 안전한 쪽(NONE = 소등)으로 떨어뜨리고 그 결과를 그대로 전송하는 편이 낫다.
     *
     * 이것이 요구사항의 "잘못된 값을 임의로 변환하지 말 것"과 어긋나지 않는 이유: 그 규칙은
     * 회선 위 위험도를 해석하는 Slave 쪽 규칙이고, Slave 는 지금도 0/1/2 밖의 값을
     * VERDICT_BAD_RISK 로 거절한다. 여기는 RPi 가 보낸 상위 값을 우리 규약으로 정규화하는
     * 자리이고, 정규화 결과(항상 0/1/2)만 회선에 나간다. */
    if ((event.risk_level != (uint8_t)VEDA_RISK_NONE) &&
        (event.risk_level != (uint8_t)VEDA_RISK_WARNING) &&
        (event.risk_level != (uint8_t)VEDA_RISK_DANGER))
    {
      ++stat_bad_risk;
      stat_last_bad_risk = event.risk_level;
    }

    /* risk_to_status() 는 규약 밖의 등급을 안전한 쪽(NONE)으로 떨어뜨린다. 회선에 나가는
     * 것은 항상 이 정규화 결과이지 event.risk_level 원본이 아니다. */
    risk_to_status(event.risk_level, &next);

    /* 같은 등급이 다시 와도 desired 한 칸을 덮어쓸 뿐이라 회선에는 아무것도 나가지 않는다.
     * 예전에는 여기서 곧바로 재전송했는데(RPi의 checkChannelMismatch 복구를 그대로 흘려
     * 보내려는 의도였다), 그 때문에 DANGER 연발이 전부 버스에 쌓여 뒤늦게 몰아서 재생됐다.
     * 복구 책임은 이제 sched_task 가 진다 -- ACK 재시도와 SCHED_REFRESH_MSEC 주기 재전송이
     * 그 자리를 대신한다. 둘 중 하나라도 빠지면 이 최적화는 위험하다. */
    if (next.risk_level == channel_ctrl[channel_index].desired_risk)
    {
      ++stat_cmd_coalesced;
    }
    else if (next.risk_level == channel_ctrl[channel_index].applied_risk)
    {
      /* 값이 바뀌긴 했지만 이미 Slave 에 그 상태가 들어가 있다(예: WARNING 으로 갔다가
       * 서비스되기 전에 NONE 으로 되돌아온 경우). 회선에 내보낼 것이 없으므로 이건
       * '지연'이 아니다 -- 여기서 걸어 두면 다음 주기 리프레시(2초)에야 풀려서
       * lat_max 가 2000ms 로 잘못 찍힌다. 계측을 취소한다. */
      channel_ctrl[channel_index].lat_pending = 0U;
    }
    else
    {
      /* 값이 '바뀐' 순간만 지연 계측의 시작점이다. 같은 등급의 반복 명령까지 재면
       * 스케줄러가 흡수해 회선에 안 나가는 것을 지연으로 잘못 세게 된다. */
      channel_ctrl[channel_index].desired_tick = osKernelGetTickCount();
      channel_ctrl[channel_index].lat_risk = next.risk_level;
      channel_ctrl[channel_index].lat_pending = 1U;
    }

    channel_ctrl[channel_index].desired_risk = next.risk_level;
  }
}

/* ===========================================================================
 * 스케줄러 (유일한 RS-485 송신자)
 * ---------------------------------------------------------------------------
 * 스케줄러 로직은 두 개의 .inc 로 나뉘어 있다. 추상화가 아니라 검증을 위해서다 --
 * 호스트 테스트가 같은 소스를 그대로 포함해 펌웨어와 갈라지지 않게 한다.
 *   - sched_select.inc  : "이번 틱에 누구를 고르는가" (pick_pending). tools/test_sched_select.c
 *   - sched_dispatch.inc: "고른 뒤 상태가 어떻게 바뀌는가" (dispatch_channel, sched_tick).
 *                         tools/test_sched_loop.c 가 가짜 Slave 로 이 상태 전이를 검증한다.
 * 스케줄러를 고치면 해당 .inc 를 포함하는 호스트 테스트도 함께 돌릴 것.
 * =========================================================================== */
#include "sched_select.inc"
#include "sched_dispatch.inc"

/**
 * @brief SCHED_TICK_MSEC마다 채널 하나씩 RS-485로 내보낸다. huart1 송신자는 이 태스크뿐이다.
 * @details 한 틱에 한 채널만 보내는 것이 핵심이다. 버스 점유가 결정적이 되고, Slave의 단일
 * ACK 슬롯(ack_pending)이 다음 명령에 덮어써지지 않을 만큼의 간격도 여기서 보장된다.
 *
 * 우선순위는 DANGER 먼저다. 경보를 켜는 일이 끄는 일보다 항상 급하기 때문이다. 두 패스가
 * 같은 커서를 공유하므로 DANGER 채널이 여럿이어도 자기들끼리 라운드로빈이 돌고, DANGER가
 * 계속 들어와도 NONE/WARNING이 밀리는 것은 한 틱(20ms)뿐이라 눈에 보이지 않는다.
 *
 * ACK 타임아웃(100ms)이 틱(20ms)보다 길다. 타임아웃이 나면 osDelayUntil 의 목표 시각이 이미
 * 지나 있어 즉시 반환하고 백투백으로 다음 채널을 잡는다. 최악(4채널 전부 무응답)이라도
 * 커서가 매 회차 전진하므로 굶는 채널은 없다.
 */
static void StartSchedTask(void *argument)
{
  uint32_t tick = osKernelGetTickCount();
  uint8_t index;

  (void)argument;

  /* 수신 상태(DE=Low)로 만들어 두고 시작한다. 명령을 보내는 순간에만 버스를 잡아야
   * Slave의 ACK가 회선에 실린다. */
  rs485_de_init();

  /* Slave의 ACK 수신을 연다. 이후 재무장은 각 콜백이 담당한다.
   * 스케줄러가 뜬 뒤에 여는 이유는 하행(USART6)과 같다 -- 큐가 준비되기 전에 바이트가
   * 도착하면 갈 곳이 없다. */
  (void)HAL_UART_Receive_IT(&huart1, &rs485_rx_byte, 1U);

  /* 부팅 직후 모든 경광등을 명시적으로 끈다. 리셋 전 상태가 남아 있으면 우리가 보고하는
   * 상태(전부 NONE)와 실제 경광등이 어긋난 채로 시작하게 된다.
   * 여기서는 ACK를 확인하지 않는다. Slave가 아직 부팅 배너(LD2 점멸)를 찍는 중이라
   * 응답할 준비가 안 됐을 수 있고, 이 명령은 어차피 '이미 꺼져 있음'을 확정하는 용도다.
   * 확인하지 않았으므로 last_tx_tick 을 지금으로 찍어 둔다 -- 그래야 첫 리프레시가
   * SCHED_REFRESH_MSEC 뒤에 와서 이 소등을 한 번 더 확정한다. */
  for (index = 0U; index < CHANNEL_COUNT; ++index)
  {
    (void)send_channel_command(index, (uint8_t)VEDA_RISK_NONE);
    channel_ctrl[index].applied_risk = (uint8_t)VEDA_RISK_NONE;
    channel_ctrl[index].last_tx_tick = osKernelGetTickCount();
  }
  drain_ack_queue();

  for (;;)
  {
    tick += SCHED_TICK_MSEC;
    (void)osDelayUntil(tick);

    /* 한 틱의 선택+송신 로직은 sched_dispatch.inc 의 sched_tick() 에 있다. 호스트 테스트
     * (tools/test_sched_loop.c)가 같은 함수를 포함해 검증하므로, 여기서 풀어 쓰면 테스트와
     * 실기가 갈라진다. 이 루프는 20ms 주기(osDelayUntil)만 책임진다. */
    sched_tick();
  }
}

/**
 * @brief 상행 큐를 비우는 유일한 USART6 송신자.
 */
static void StartTxTask(void *argument)
{
  veda_uplink_packet_t packet;

  (void)argument;

  for (;;)
  {
    if (osMessageQueueGet(uplink_queue, &packet, NULL, osWaitForever) == osOK)
    {
      uplink_send(&packet);
    }
  }
}

/**
 * @brief HEARTBEAT_INTERVAL_MSEC마다 채널마다 한 장씩 HEARTBEAT를 올린다.
 * @details RPi는 채널별로 마지막 HEARTBEAT 시각을 따로 들고 있으므로(lastHeartbeatAt_),
 * 한 장으로 전체를 대표할 수 없다. 채널 수만큼 보내야 한다.
 *
 * osDelay가 아니라 osDelayUntil을 쓴다. osDelay는 '깨어난 뒤부터' 다시 세기 때문에
 * 루프 본문 시간이 매 주기 누적되고, 주기가 조금씩 늘어나 결국 dead 판정에 걸린다.
 */
static void StartHeartbeatTask(void *argument)
{
  uint32_t next_wake = osKernelGetTickCount();
  uint8_t channel_index;

  (void)argument;

  for (;;)
  {
    next_wake += HEARTBEAT_INTERVAL_MSEC;
    (void)osDelayUntil(next_wake);

    for (channel_index = 0U; channel_index < CHANNEL_COUNT; ++channel_index)
    {
      enqueue_uplink(channel_index, (uint8_t)VEDA_UPLINK_REASON_HEARTBEAT);
    }
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  /* 관제 서버(Raspberry Pi) 회선. CubeMX가 만들지 않는 포트라 직접 초기화한다. */
  rpi_uart_init();
  /* 첫 HEARTBEAT가 나가기 전에 상태가 정해져 있어야 한다. */
  channel_status_init();
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  channel_status_mutex = osMutexNew(&channel_status_mutex_attributes);
  if (channel_status_mutex == NULL)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  rpi_rx_queue = osMessageQueueNew(RPI_RX_QUEUE_DEPTH, sizeof(uint8_t), NULL);
  cmd_queue = osMessageQueueNew(CMD_QUEUE_DEPTH, sizeof(veda_risk_event_t), NULL);
  uplink_queue = osMessageQueueNew(UPLINK_QUEUE_DEPTH, sizeof(veda_uplink_packet_t), NULL);
  ack_queue = osMessageQueueNew(ACK_QUEUE_DEPTH, sizeof(rs485_ack_t), NULL);
  if ((rpi_rx_queue == NULL) || (cmd_queue == NULL) || (uplink_queue == NULL) || (ack_queue == NULL))
  {
    /* 힙 부족. configTOTAL_HEAP_SIZE를 늘리거나 큐 깊이를 줄일 것. 여기서 그냥 진행하면
     * 이후 모든 큐 연산이 조용히 실패해 "아무 일도 일어나지 않는" 상태가 된다. */
    Error_Handler();
  }
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  rxTaskHandle = osThreadNew(StartRxTask, NULL, &rxTask_attributes);
  ctrlTaskHandle = osThreadNew(StartCtrlTask, NULL, &ctrlTask_attributes);
  schedTaskHandle = osThreadNew(StartSchedTask, NULL, &schedTask_attributes);
  txTaskHandle = osThreadNew(StartTxTask, NULL, &txTask_attributes);
  heartbeatTaskHandle = osThreadNew(StartHeartbeatTask, NULL, &heartbeatTask_attributes);
  if ((rxTaskHandle == NULL) || (ctrlTaskHandle == NULL) ||
      (txTaskHandle == NULL) || (heartbeatTaskHandle == NULL))
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, RELAY_A_Pin|LD2_Pin|NEOPIXEL_DIN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : RELAY_A_Pin LD2_Pin */
  GPIO_InitStruct.Pin = RELAY_A_Pin|LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : NEOPIXEL_DIN_Pin */
  GPIO_InitStruct.Pin = NEOPIXEL_DIN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(NEOPIXEL_DIN_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  감시 태스크. 통신 자체에는 관여하지 않고 상태만 밖으로 드러낸다.
  * @param  argument: Not used
  * @retval None
  * @details 하는 일은 세 가지다.
  *  1) LD2 점멸 -- 하행이 살아있으면 빠르게(0.5s), 조용하면 느리게(2s). USB 없이도 링크
  *     상태를 눈으로 구분할 수 있게 하기 위한 것이다.
  *  2) USART2(ST-Link VCP)로 진단 카운터 주기 출력. 하행이 바이너리가 되면서 USART6는
  *     사람이 읽을 수 없게 됐고, 프레임이 버려질 때 Master는 밖에서 볼 흔적을 남기지 않는다.
  *  3) MASTER_SELFTEST_ENABLED일 때 순서표대로 위험 이벤트를 스스로 큐에 넣는다.
  *
  * LD2와 huart2를 이 태스크만 만진다. 여러 태스크가 나눠 쓰면 그때부터 보호가 필요해지는데,
  * 진단 수단 때문에 실시간 경로에 락을 들이는 것은 순서가 뒤바뀐 일이다.
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN 5 */
  uint32_t now = osKernelGetTickCount();
  uint32_t next_log = now + SUPERVISOR_LOG_INTERVAL_MSEC;
  uint32_t next_blink = now;
  uint32_t downlink_age;
#if MASTER_SELFTEST_ENABLED
  uint32_t next_selftest = now + RS485_SEND_INTERVAL_MSEC;
  veda_risk_event_t injected;
#endif

  (void)argument;

  /* 부팅 배너. RPi와 반드시 일치해야 하는 설정을 그대로 찍는다 -- 이 세 줄이 어긋난 채로
   * 하루를 태우는 일이 없도록. */
  dbg_print("\r\n=== VEDA MASTER READY ===\r\n");
  dbg_print(" channels    = ");
  dbg_print_u32(CHANNEL_COUNT);
  dbg_print(" (rpi channel_id base = ");
  dbg_print_u32(RPI_CHANNEL_ID_BASE);
  dbg_print(")\r\n heartbeat   = ");
  dbg_print_u32(HEARTBEAT_INTERVAL_MSEC);
  dbg_print(" ms/channel\r\n downlink    = ");
  dbg_print_u32((uint32_t)sizeof(veda_downlink_frame_t));
  dbg_print("B / uplink = ");
  dbg_print_u32((uint32_t)sizeof(veda_uplink_frame_t));
  dbg_print("B\r\n\r\n");

  for (;;)
  {
    osDelay(SUPERVISOR_TICK_MSEC);
    now = osKernelGetTickCount();

    /* last_downlink_tick == 0 이면 부팅 후 아직 한 장도 못 받은 것이다. */
    downlink_age = (last_downlink_tick == 0U) ? LINK_IDLE_WARN_MSEC
                                              : (now - last_downlink_tick);

    if ((now - next_blink) < 0x80000000U)   /* 틱 접힘에 안전한 시각 비교 */
    {
      HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
      next_blink = now + ((downlink_age >= LINK_IDLE_WARN_MSEC) ? LD2_BLINK_IDLE_MSEC
                                                                : LD2_BLINK_ALIVE_MSEC);
    }

#if MASTER_SELFTEST_ENABLED
    if ((now - next_selftest) < 0x80000000U)
    {
      /* 실제 하행 경로와 같은 큐로 넣는다. ctrl_task 이후는 운영 경로와 완전히 동일하다. */
      (void)memset(&injected, 0, sizeof(injected));
      injected.channel_id = (uint8_t)(test_sequence[test_step_index].channel_index + RPI_CHANNEL_ID_BASE);
      injected.risk_level = test_sequence[test_step_index].risk_level;
      injected.timestamp_ms = veda_now_ms();
      injected.dist_mm = VEDA_DIST_MM_NONE;
      (void)osMessageQueuePut(cmd_queue, &injected, 0U, 0U);

#if MASTER_DEBUG_LOG
      /* 지금 어느 채널을 미는지 밝힌다. 이게 없으면 경광등만 보고 있게 되어, 불이 안 들어올 때
       * "이 보드가 담당하지 않는 채널이라 정상적으로 무시한 것"과 "받아야 하는데 못 받은 것"을
       * 구분할 수 없다. Slave의 USART2 로그(RX ... -> 판정)와 나란히 놓고 보면 확실하다. */
      {
        const uint8_t step_index = test_sequence[test_step_index].channel_index;

        dbg_print("[selftest] S");
        dbg_print_u32((uint32_t)((step_index / CHANNELS_PER_SLAVE) + 1U));
        dbg_print(":CH");
        dbg_print_u32((uint32_t)(step_index + 1U));
        dbg_print(" -> R");
        dbg_print_u32((uint32_t)test_sequence[test_step_index].risk_level);
        dbg_print("\r\n");
      }
#endif

      ++test_step_index;
      if (test_step_index >= TEST_SEQUENCE_LENGTH)
      {
        test_step_index = 0U;
      }
      next_selftest = now + RS485_SEND_INTERVAL_MSEC;
    }
#endif /* MASTER_SELFTEST_ENABLED */

    if ((now - next_log) < 0x80000000U)
    {
      dbg_print("[stat] ");
      dbg_print_stat("ok", stat_frames_ok);
      dbg_print_stat("bad", stat_frames_bad);
      dbg_print_stat("bad_ch", stat_bad_channel);
      dbg_print_stat("last_bad_ch", stat_last_bad_channel);
      /* bad_risk 가 올라가면 RPi가 0/1/2 밖의 등급을 보내고 있거나 회선이 깨진 것이다.
       * last_bad_risk 에 마지막 값이 그대로 남으므로 둘을 구분할 수 있다. */
      dbg_print_stat("bad_risk", stat_bad_risk);
      dbg_print_stat("last_bad_risk", stat_last_bad_risk);
      dbg_print_stat("rx_drop", stat_rx_bytes_dropped);
      dbg_print_stat("cmd_drop", stat_cmd_dropped);
      /* coalesce 가 크게 올라가는 것은 정상이다 -- RPi가 같은 등급을 반복 송신 중이고
       * 스케줄러가 그걸 흡수하고 있다는 뜻이다. 예전 구조에서 버스에 쌓이던 양이다.
       * retry 가 계속 올라가면 그 채널의 Slave가 응답하지 않는 것이다. */
      dbg_print_stat("coalesce", stat_cmd_coalesced);
      dbg_print_stat("retry", stat_sched_retry);
      dbg_print_stat("refresh", stat_sched_refresh);
      /* 반응 지연 상한. lat_dgr 이 이 장비의 '경보가 켜지기까지'의 최악값이다.
       * lat_dgr 이 lat_max 보다 크면 DANGER 우선 패스가 duty 를 못 하고 있는 것이다. */
      dbg_print_stat("lat_max", stat_lat_max);
      dbg_print_stat("lat_dgr", stat_lat_danger_max);
      dbg_print_stat("485_fail", stat_rs485_tx_fail);
      /* ack_ok가 0에서 안 올라가면 Slave의 응답이 회선에 실리지 못하는 것이다
       * (트랜시버 DE/RE 고정 배선 등). 그 상태에서 RS485_ACK_REQUIRED를 1로 올리면
       * 모든 채널이 영구 불일치로 보고되므로, 이 값을 먼저 확인할 것. */
      dbg_print_stat("ack_ok", stat_ack_ok);
      dbg_print_stat("ack_bad", stat_ack_bad);
      dbg_print_stat("ack_to", stat_ack_timeout);
      dbg_print_stat("ack_mis", stat_ack_mismatch);
      dbg_print_stat("up_tx", stat_uplink_sent);
      dbg_print_stat("up_fail", stat_uplink_tx_fail);
      dbg_print_stat("up_drop", stat_uplink_dropped);
      dbg_print("dl_age=");
      if (last_downlink_tick == 0U)
      {
        dbg_print("never");
      }
      else
      {
        dbg_print_u32(downlink_age);
        dbg_print("ms");
      }
      dbg_print("\r\n");

      dbg_print_stack_line();

      next_log = now + SUPERVISOR_LOG_INTERVAL_MSEC;
    }
  }
  /* USER CODE END 5 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
