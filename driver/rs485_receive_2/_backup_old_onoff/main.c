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

#include "neopixel.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/**
 * 이 보드의 RS-485 주소. Slave #1은 1, Slave #2는 2로 바꿔 빌드한다.
 * RS-485는 주소를 자동으로 알려주지 않으므로 보드마다 사람이 정해 심어야 한다.
 *
 *   Slave #1 = CH1, CH2 담당   (경광등 = CH1)
 *   Slave #2 = CH3, CH4 담당   (경광등 = CH3)
 *
 * 어느 채널이 경광등을 울리는지는 아래 channel_output[] 표가 결정한다. 이 값을 바꾸면
 * 그 표도 함께 갈리므로 두 곳을 따로 맞출 필요는 없다.
 * 부팅 시 LD2가 이 번호만큼 깜빡이므로, 구운 뒤 보드만 보고 확인할 수 있다.
 */
#define MY_SLAVE_ID 2U

/**
 * 이 보드가 담당하는 Channel 수. Master의 같은 이름 매크로와 반드시 같아야 한다.
 * 한쪽만 바꾸면 라우팅이 조용히 어긋나 남의 채널을 받거나 자기 채널을 놓친다.
 *
 * 늘리려면 이 값과 아래 channel_output[] 표를 함께 늘리면 된다 -- 나머지 코드는
 * 전부 이 둘에서 파생되므로 손댈 곳이 없다.
 */
#define CHANNELS_PER_SLAVE 2U

/**
 * 이 Slave가 담당하는 첫 Channel 번호. 보드 한 대가 Channel 두 개를 연속으로 맡는다.
 *
 *   Slave #1 -> CH1(RELAY_A/PA0), CH2(RELAY_B/PA1)
 *   Slave #2 -> CH3(RELAY_A/PA0), CH4(RELAY_B/PA1)
 *
 * Master의 send_channel_command()가 같은 규칙으로 슬레이브 주소와 채널 숫자를 따로
 * 계산하므로 양쪽이 이 식으로 맞물린다. 슬레이브 번호와 채널 번호는 같지 않다.
 */
#define MY_FIRST_CHANNEL (((MY_SLAVE_ID - 1U) * CHANNELS_PER_SLAVE) + 1U)
/** 담당 마지막 Channel 번호 (포함) */
#define MY_LAST_CHANNEL (MY_FIRST_CHANNEL + CHANNELS_PER_SLAVE - 1U)
/** RS-485 프레임 한 줄의 최대 길이. "S1:CH1:OFF"가 10자다. */
#define RS485_LINE_BUFFER_SIZE 16U
/** 이 시간만큼 수신이 없으면 조립 중이던 프레임을 폐기해 재동기화한다. */
#define RS485_IDLE_RESET_MSEC 50U

/**
 * USART2(ST-Link VCP, 115200)로 진단 로그를 낸다. 0으로 두면 코드가 남지 않는다.
 *
 * 이 로그가 필요한 이유: 프레임이 버려질 때 이 Slave는 아무 흔적도 남기지 않으므로,
 * "명령이 오지 않았다 / 왔지만 내 것이 아니라 무시했다 / 받아서 GPIO까지 썼다"를
 * 밖에서 구분할 수 없다. 보드마다 MY_SLAVE_ID를 손으로 바꿔 굽는 구조라
 * 어느 보드가 몇 번으로 구워졌는지도 확인할 방법이 없었다.
 */
#define SLAVE_DEBUG_LOG 1

/** 부팅 시 LD2를 MY_SLAVE_ID번 깜빡이는 간격. USB 연결 없이 보드 번호를 눈으로 확인한다. */
#define SLAVE_ID_BLINK_MSEC 250U

/**
 * 부저를 담당 채널과 함께 울릴지 여부. 0으로 두면 부저 핀을 아예 잡지 않는다.
 *
 * 배선 규격상 부저는 채널마다 하나씩 있다(A = PB5/D4, B = PA8/D7). 규격의 "경광등과 부저는
 * 같은 채널에서 동시에 ON/OFF" 를 그대로 따르므로, 각 부저는 자기 채널만 보고 운다 --
 * 옆 채널이 켜져도 조용하다.
 *
 * 지금 회선 규약은 위험 등급을 싣지 않아 Slave가 WARNING과 DANGER를 구분할 수 없다
 * (Master의 ctrl_task가 risk_level != NONE 이면 무조건 ON을 보낸다). 그래서 경광등과
 * 정확히 같은 조건으로 동작한다 -- DANGER 에서만 울리게 하려면 회선 규약부터 확장해야 한다.
 */
#define BUZZER_ENABLED 1

/**
 * 부저 모듈의 극성. 울릴 때 핀을 High로 둘지 Low로 둘지. 두 채널이 같은 모듈을 쓴다고 본다.
 *
 * 1 = active-high (핀 High일 때 울림).
 * 0 = active-low  (핀 Low일 때 울림). 능동 부저 모듈 중에 이런 것이 흔하다.
 *
 * 규격서도 "부저 모듈의 Active-High/Active-Low 동작을 실물에서 확인한다"고 못박아 두었다.
 * 잘못 잡으면 부팅하자마자 계속 울리고 명령이 와도 안 그친다. 그 증상이 보이면 배선을
 * 의심하기 전에 이 값부터 뒤집어 볼 것.
 */
#define BUZZER_ACTIVE_HIGH 1

/**
 * NeoPixel(WS2812) 줄 조명을 담당 채널과 함께 켤지 여부. 0으로 두면 핀을 아예 잡지 않는다.
 *
 * 부저와 마찬가지로 채널마다 하나씩 있다(A = PA6/D12, B = PB10/D6). 규격의 "NeoPixel은
 * 채널별 상태에 따라 색상 표시" 를 따라 각 스트립이 자기 채널 색만 낸다.
 *
 * 경광등(릴레이)은 외부 전원을 물리므로 배선이 틀리면 아무 반응이 없는데, 줄 조명은 색까지
 * 보이므로 "명령이 도달했고 적용까지 끝났다"를 가장 빨리 확인하는 수단이 된다.
 */
#define NEOPIXEL_ENABLED 1

/**
 * 줄 조명 한 줄에 보낼 픽셀 수. 지금 달린 스트립(SZH-LD314)은 LED 9개짜리다.
 *
 * 9로 두면 어느 쪽이든 맞는다:
 *   - LED 하나하나가 개별 제어되는 스트립이면 픽셀 9개 = 정확히 일치
 *   - 12V 스트립처럼 컨트롤러 하나가 LED 3개를 묶는 물건이면 실제 픽셀은 3개인데,
 *     넉넉히 보내는 것은 무해하다 -- 남는 데이터는 마지막 컨트롤러를 지나 그냥 버려진다
 *
 * 반대로 실제보다 적게 잡으면 그 뒤쪽이 갱신되지 않아 꺼진 채로 남는다.
 * 그래서 애매할 때는 넉넉히 잡는 쪽이 안전하다.
 *
 * 예전에는 규격 최대치인 300으로 두었는데, 그러면 한 번 갱신에 약 9ms 가 걸린다.
 * 9픽셀이면 약 0.6ms 로 줄어 ACK 응답과 RS-485 수신에 주는 부담도 함께 줄어든다.
 *
 * 스트립을 더 긴 것으로 바꾸면 이 값만 고치면 된다(라이브러리 상한은 300).
 */
#define NEOPIXEL_PIXEL_COUNT 9U

/**
 * 상태별 색 (RGB 순, 규격서 상태 색상표의 값).
 *
 * 규격서는 안전(0,255,0) / 주의(255,255,0) / 위험(255,0,0) / OFF(0,0,0) 네 가지를 정의하지만,
 * 지금 RS-485 회선 규약이 ON/OFF만 싣고 위험 등급을 싣지 않아 Slave는 '주의(노랑)'를
 * 구분할 수 없다. Master의 ctrl_task가 risk_level != NONE 이면 무조건 ON을 보내기 때문이다.
 * 그래서 이 보드가 낼 수 있는 색은 아래 둘뿐이다 -- 노랑까지 쓰려면 회선 규약부터 확장해야
 * 한다(부저가 WARNING/DANGER를 구분하지 못하는 것과 같은 이유다).
 */
#define NEOPIXEL_SAFE_R 0U     /**< 그 채널이 OFF일 때: 안전(초록) */
#define NEOPIXEL_SAFE_G 255U
#define NEOPIXEL_SAFE_B 0U
#define NEOPIXEL_ALARM_R 255U  /**< 그 채널이 ON일 때: 위험(빨강) */
#define NEOPIXEL_ALARM_G 0U
#define NEOPIXEL_ALARM_B 0U

/**
 * 명령을 적용한 뒤 Master로 ACK를 되보낸다. 0으로 두면 예전처럼 수신 전용으로 동작한다.
 *
 * 이게 없으면 Master는 "회선에 바이트를 밀어넣었다"까지만 알고 이 보드가 실제로 GPIO를
 * 썼는지 모른다. 그 상태에서는 이 보드의 전원이 꺼져 있거나 MY_SLAVE_ID가 어긋나 프레임을
 * 통째로 버려도 Master가 RPi에게 "정상 점등 중"이라고 보고한다 -- 어디도 에러를 남기지
 * 않는데 경광등만 안 울리는 상황이 된다. ACK는 그 구간을 메우는 유일한 증거다.
 *
 * 프레임 형식: "A<슬레이브>:CH<채널>:<ON|OFF>\n"
 * Master 명령('S')과 첫 글자로 구분되므로, 버스를 공유하는 다른 Slave가 이 ACK를 명령으로
 * 오해하지 않는다(process_rs485_line()이 line[0] != 'S' 를 형식 오류로 버린다).
 */
#define SLAVE_ACK_ENABLED 1
/** ACK 프레임 조립 버퍼. "A1:CH1:OFF\n"이 11자다. */
#define RS485_ACK_BUFFER_SIZE 16U
/** ACK 송신 타임아웃 */
#define RS485_ACK_TX_TIMEOUT_MSEC 50U

/**
 * RS-485 트랜시버의 송신 드라이버(DE/RE)를 GPIO로 제어할지 여부.
 *
 * 0 = 방향 제어 없음. 자동 방향 전환 트랜시버(MAX13487 등)이거나, DE/RE가 고정 배선된
 *     구성이다. 고정 배선(이 보드가 수신 전용)이라면 아래 ACK 송신이 회선에 실리지 못하고
 *     Master의 [stat] ack_ok 가 0에서 올라가지 않는다 -- 그걸로 배선 방식을 판별할 수 있다.
 * 1 = MAX485 등 수동 트랜시버. DE와 RE를 묶어 RS485_DE_Pin 에 연결한 뒤 이 값을 1로 둘 것.
 *
 * PA4를 고른 이유: 이 보드에서 쓰이지 않는 유일한 인접 핀이다(PA0/PA1 릴레이, PA2/PA3
 * USART2, PA5 LD2, PA8 네오픽셀, PA9/PA10 USART1, PA13/PA14 SWD가 이미 점유).
 */
#define RS485_DE_ENABLED 0
#define RS485_DE_Pin GPIO_PIN_4
#define RS485_DE_GPIO_Port GPIOA

#if (RS485_DE_ENABLED != 0) && (SLAVE_ACK_ENABLED == 0)
/* DE 핀을 잡는 곳이 rs485_de_init()뿐이고 그 함수는 ACK 기능 안에 들어 있다. ACK를 끈 채로
 * DE만 켜면 핀이 초기화되지 않은 채 떠 있어 트랜시버가 어느 방향인지 알 수 없게 된다. */
#error "RS485_DE_ENABLED는 SLAVE_ACK_ENABLED가 1일 때만 쓸 수 있다"
#endif
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
/** 조립 중인 RS-485 프레임 */
static char rs485_line[RS485_LINE_BUFFER_SIZE];
static volatile uint8_t rs485_line_length;
/** 버퍼를 넘긴 줄은 개행이 올 때까지 통째로 버린다는 표시 */
static volatile uint8_t rs485_line_overflow;
/** USART1 인터럽트 수신용 1바이트 버퍼 */
static uint8_t rs485_rx_byte;
static volatile uint32_t rs485_last_rx_msec;

#if SLAVE_ACK_ENABLED
/**
 * ISR이 적용을 끝낸 명령을 ACK 송신용으로 태스크에 넘기는 자리.
 * RS-485 송신은 블로킹(10바이트 = 약 870us)이라 ISR 안에서 하면 그동안 다음 바이트를
 * 놓쳐 오버런이 난다. debug_line과 같은 방식으로 ISR은 값과 플래그만 세우고
 * 실제 송신은 StartDefaultTask가 맡는다.
 *
 * 앞 ACK를 아직 못 보냈으면 이번 것은 버린다(플래그가 곧 락 역할을 한다). Master는
 * 명령 하나를 보내고 ACK를 기다린 뒤 다음을 보내므로 정상 운용에서는 겹치지 않는다.
 * 부팅 직후 Master가 4채널 OFF를 연속으로 쏠 때만 일부가 버려지는데, 그 구간의 ACK는
 * Master도 확인하지 않는다.
 */
static volatile uint8_t ack_pending;
static volatile uint8_t ack_channel;
static volatile uint8_t ack_turn_on;
#endif

#if SLAVE_DEBUG_LOG
/**
 * ISR이 해석을 끝낸 줄과 그 판정을 태스크로 넘기는 자리.
 * USART2 송신은 블로킹이라 ISR 안에서 하면 RS-485 수신이 밀려 오버런이 난다.
 * ISR은 복사와 플래그만 세우고, 출력은 StartDefaultTask가 맡는다.
 */
static char debug_line[RS485_LINE_BUFFER_SIZE + 1U];
static volatile uint8_t debug_verdict;
static volatile uint8_t debug_ready;
#endif
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART1_UART_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/** process_rs485_line()의 판정 코드. 프레임이 어디서 버려졌는지 밖에서 알기 위한 것이다.
 *  SLAVE_DEBUG_LOG가 0이어도 process_rs485_line()의 반환형으로 쓰이므로 항상 정의한다. */
#define VERDICT_APPLIED      0U  /**< 수용해서 GPIO까지 썼다 */
#define VERDICT_BAD_FORMAT   1U  /**< "S<n>:CH<n>:<ON|OFF>" 형식이 아니다 */
#define VERDICT_OTHER_SLAVE  2U  /**< 형식은 맞지만 다른 Slave 주소다 */
#define VERDICT_BAD_ACTION   3U  /**< ON/OFF 자리가 아니다 */
#define VERDICT_OTHER_CHAN   4U  /**< 내 주소지만 내가 담당하지 않는 Channel이다 */

#if SLAVE_DEBUG_LOG
static const char *const verdict_text[] = {
  "APPLIED (relay+buzzer+strip)",
  "dropped: bad format",
  "dropped: another slave",
  "dropped: bad ON/OFF",
  "dropped: not my channel",
};

/**
 * @brief USART2(ST-Link VCP)로 문자열을 내보낸다. 태스크 문맥에서만 부를 것.
 * @details 블로킹 송신이라 ISR에서 부르면 RS-485 수신 인터럽트가 밀려 오버런이 난다.
 */
static void slave_print(const char *text)
{
  (void)HAL_UART_Transmit(&huart2, (const uint8_t*)text, (uint16_t)strlen(text), 100U);
}

/** @brief 한 자리 숫자를 찍는다. 슬레이브 번호와 채널 번호는 모두 1~9라 이걸로 충분하다. */
static void slave_print_digit(uint8_t value)
{
  const char text[2] = { (char)('0' + (value % 10U)), '\0' };
  slave_print(text);
}
#endif /* SLAVE_DEBUG_LOG */

/**
 * @brief 한 Channel에 딸린 하드웨어 한 벌.
 * @details 배선 규격표의 "채널 구성"을 그대로 옮긴 것이다. 확장 지점은 이 표 하나이고,
 * 나머지 코드는 표를 조회만 하므로 손댈 필요가 없다.
 *
 *   채널 A : PA0 경광등 릴레이 + PB5 부저 + PA6 NeoPixel
 *   채널 B : PA1 경광등 릴레이 + PA8 부저 + PB10 NeoPixel
 *
 * 예전에는 부저와 줄 조명이 보드에 하나씩뿐이라 '담당 채널 중 하나라도 ON'이면 함께
 * 울리고 켜졌다. 규격이 "경광등과 부저는 같은 채널에서 동시에 ON/OFF", "NeoPixel은 채널별
 * 상태에 따라 색상 표시" 로 정해져 있어, 세 가지를 한 항목에 묶어 채널 단위로 움직이게 했다.
 *
 * 이 표만 고쳐서 되는 것들:
 *   - 채널 <-> 하드웨어 대응 바꾸기  : 항목의 channel 값만 교체
 *   - 담당 채널 중 일부만 배선하기    : 항목을 빼면 그 채널은 OTHER_CHAN 으로 거절된다
 *   - 보드당 채널 수 늘리기          : CHANNELS_PER_SLAVE 와 함께 항목을 추가
 */
typedef struct
{
  uint8_t channel;            /**< 이 한 벌이 담당하는 Channel 번호 (1-based, 회선 위 표현과 같다) */
  GPIO_TypeDef *relay_port;   /**< 경광등 릴레이 (2N7000 게이트) */
  uint16_t relay_pin;
  GPIO_TypeDef *buzzer_port;  /**< 능동 부저 모듈의 S 단자 */
  uint16_t buzzer_pin;
  neopixel_channel_t strip;   /**< NeoPixel 줄 조명 채널 */
  const char *set_name;       /**< 규격표상의 채널 이름 ("A" / "B") */
  const char *wiring;         /**< 배선을 눈으로 대조하기 위한 핀 이름. 부팅 배너에 그대로 찍힌다 */
} channel_output_t;

/**
 * 보드별 채널 <-> 하드웨어 대응표. 표의 순서에는 의미가 없고, 채널 번호로 찾는다
 * (channel_output_index()).
 *
 * MY_SLAVE_ID 로 갈라 놓은 이유: 보드마다 담당하는 절대 채널 번호가 다르기 때문이다.
 * 하드웨어 핀은 모든 보드가 같다(규격표가 공통 핀맵이라고 못박아 두었다) -- 갈리는 것은
 * 채널 번호뿐이다.
 *
 *   Slave #1 -> CH1 = A 세트, CH2 = B 세트
 *   Slave #2 -> CH3 = A 세트, CH4 = B 세트
 *
 * 각 보드의 표를 절대 채널 번호로 그대로 적어 두었다 -- 어느 보드가 무엇을 울리는지 이
 * 블록만 보면 알 수 있고, 한쪽을 고쳐도 다른 쪽에 영향이 없다.
 *
 * wiring/set_name 문자열은 USART2 로 그대로 나가므로 ASCII 로 적을 것.
 */
#if MY_SLAVE_ID == 1U
static const channel_output_t channel_output[] = {
  { 1U, RELAY_A_GPIO_Port, RELAY_A_Pin, BUZZER_A_GPIO_Port, BUZZER_A_Pin,
    NEOPIXEL_CH_A, "A", "PA0/PB5/PA6" },
  { 2U, RELAY_B_GPIO_Port, RELAY_B_Pin, BUZZER_B_GPIO_Port, BUZZER_B_Pin,
    NEOPIXEL_CH_B, "B", "PA1/PA8/PB10" },
};
#elif MY_SLAVE_ID == 2U
static const channel_output_t channel_output[] = {
  { 3U, RELAY_A_GPIO_Port, RELAY_A_Pin, BUZZER_A_GPIO_Port, BUZZER_A_Pin,
    NEOPIXEL_CH_A, "A", "PA0/PB5/PA6" },
  { 4U, RELAY_B_GPIO_Port, RELAY_B_Pin, BUZZER_B_GPIO_Port, BUZZER_B_Pin,
    NEOPIXEL_CH_B, "B", "PA1/PA8/PB10" },
};
#else
/* 표가 없는 보드 번호로 구우면 이 보드는 어떤 채널에도 반응하지 않는다. 조용히 죽는 대신
 * 빌드를 세운다 -- "명령은 나가는데 아무 일도 안 일어나는" 상황을 막기 위해서다. */
#error "이 MY_SLAVE_ID 에 대한 channel_output[] 표가 없다. 위에 추가할 것"
#endif

#define CHANNEL_OUTPUT_COUNT (sizeof(channel_output) / sizeof(channel_output[0]))

/* 담당 범위보다 많은 채널을 표에 적으면 남의 채널까지 구동하게 된다. 빌드 시점에 막는다.
 * 같지 않아도 되는 이유: 담당 채널 중 일부를 아직 배선하지 않은 구성을 허용하기 위해서다.
 * 표에서 뺀 채널은 OTHER_CHAN 으로 거절되므로 조용히 사라지지 않고 로그에 남는다. */
_Static_assert(CHANNEL_OUTPUT_COUNT <= CHANNELS_PER_SLAVE,
               "channel_output[] 항목 수가 CHANNELS_PER_SLAVE 보다 많다");

/** 표에 없는 채널을 가리키는 값 */
#define CHANNEL_OUTPUT_NONE 0xFFU

/**
 * @brief Channel 번호로 출력 표의 자리를 찾는다.
 * @param channel 1-based Channel 번호
 * @retval CHANNEL_OUTPUT_NONE 이 보드가 구동하지 않는 채널이다
 * @details 항목이 두어 개뿐이라 선형 탐색으로 충분하다. ISR 문맥에서 불리므로 짧게 유지한다.
 */
static uint8_t channel_output_index(uint8_t channel)
{
  uint8_t index;

  for (index = 0U; index < (uint8_t)CHANNEL_OUTPUT_COUNT; ++index)
  {
    if (channel_output[index].channel == channel)
    {
      return index;
    }
  }

  return CHANNEL_OUTPUT_NONE;
}

/** 담당 Channel별 현재 출력 상태(0=OFF, 1=ON). 인덱스는 channel_output[]과 같다. */
static uint8_t channel_on[CHANNEL_OUTPUT_COUNT];

#if BUZZER_ENABLED
/* 부저를 울릴 때 / 그칠 때 핀에 실어야 하는 레벨. 극성은 BUZZER_ACTIVE_HIGH 하나로 정한다. */
#if BUZZER_ACTIVE_HIGH
#define BUZZER_LEVEL_ON  GPIO_PIN_SET
#define BUZZER_LEVEL_OFF GPIO_PIN_RESET
#else
#define BUZZER_LEVEL_ON  GPIO_PIN_RESET
#define BUZZER_LEVEL_OFF GPIO_PIN_SET
#endif
#endif /* BUZZER_ENABLED */

/**
 * @brief 표에 적힌 모든 채널의 릴레이/부저 핀을 출력으로 잡고 꺼진 상태로 둔다.
 * @details 표가 곧 배선이므로 초기화도 표를 돌며 한다 -- 채널을 늘리거나 핀을 옮겨도
 * 이 함수는 손댈 필요가 없다.
 *
 * RELAY_A(PA0)는 .ioc 에 있어 MX_GPIO_Init() 도 잡지만, 같은 설정을 다시 넣는 것이라
 * 무해하고 이 표를 유일한 근거로 남겨 둘 수 있다. 나머지 핀은 .ioc 에 없으므로 여기서만
 * 잡힌다 -- CubeMX 로 코드를 다시 생성해도 살아남는다.
 *
 * 핀을 출력으로 잡기 전에 꺼진 레벨을 먼저 실어 둔다. 순서가 바뀌면 초기화 순간에 릴레이가
 * 짧게 붙거나 부저가 짧게 울린다(active-low 모듈에서 특히 눈에 띈다).
 *
 * NeoPixel 은 여기서 잡지 않는다. GPIO 가 아니라 타이머 AF 로 쓰이고 초기화에 HAL_Delay()
 * 가 필요해 태스크 문맥에서 neopixel_init() 이 따로 맡는다.
 */
static void channel_hardware_init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  uint8_t index;

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

  for (index = 0U; index < (uint8_t)CHANNEL_OUTPUT_COUNT; ++index)
  {
    /* 릴레이는 2N7000 게이트를 미는 것이라 Low = 경광등 OFF 다.
     * 게이트에 10k 풀다운이 붙어 있어 이 핀이 뜨는 순간에도 릴레이는 붙지 않는다. */
    HAL_GPIO_WritePin(channel_output[index].relay_port, channel_output[index].relay_pin,
                      GPIO_PIN_RESET);
    GPIO_InitStruct.Pin = channel_output[index].relay_pin;
    HAL_GPIO_Init(channel_output[index].relay_port, &GPIO_InitStruct);

#if BUZZER_ENABLED
    HAL_GPIO_WritePin(channel_output[index].buzzer_port, channel_output[index].buzzer_pin,
                      BUZZER_LEVEL_OFF);
    GPIO_InitStruct.Pin = channel_output[index].buzzer_pin;
    HAL_GPIO_Init(channel_output[index].buzzer_port, &GPIO_InitStruct);
#endif
  }
}

#if NEOPIXEL_ENABLED
/**
 * @brief 각 줄 조명에 내보낼 상태를 태스크로 넘기는 자리. 인덱스는 channel_output[]과 같다.
 * @details ACK/디버그 로그와 같은 방식이다. neopixel_show_solid()는 전송이 끝날 때까지
 * 기다리는 블로킹 함수라(300픽셀 약 9ms) ISR 안에서 부르면 그동안 RS-485 바이트를 놓쳐
 * 오버런이 난다. ISR은 원하는 상태만 적어 두고 실제 송신은 StartDefaultTask가 맡는다.
 *
 * ACK와 달리 '앞의 것을 아직 못 보냈으면 버린다'로 두지 않았다. 줄 조명은 이벤트가 아니라
 * 마지막 상태만 맞으면 되므로, 나중 값이 앞 값을 덮어쓰는 편이 옳다.
 */
static volatile uint8_t strip_alarm_on[CHANNEL_OUTPUT_COUNT];
static volatile uint8_t strip_update_pending[CHANNEL_OUTPUT_COUNT];

/**
 * @brief 표의 한 자리에 해당하는 줄 조명을 지금 상태 색으로 칠한다. 태스크 문맥 전용.
 */
static void strip_show(uint8_t index, uint8_t alarm_on)
{
  if (alarm_on != 0U)
  {
    (void)neopixel_show_solid(channel_output[index].strip, NEOPIXEL_PIXEL_COUNT,
                              NEOPIXEL_ALARM_R, NEOPIXEL_ALARM_G, NEOPIXEL_ALARM_B);
  }
  else
  {
    (void)neopixel_show_solid(channel_output[index].strip, NEOPIXEL_PIXEL_COUNT,
                              NEOPIXEL_SAFE_R, NEOPIXEL_SAFE_G, NEOPIXEL_SAFE_B);
  }
}

/**
 * @brief 예약된 줄 조명 상태를 실제로 송신한다. 태스크 문맥에서만 부를 것.
 * @details 보낼 것이 없으면 아무것도 하지 않는다.
 */
static void neopixel_service(void)
{
  uint8_t index;

  for (index = 0U; index < (uint8_t)CHANNEL_OUTPUT_COUNT; ++index)
  {
    uint8_t alarm_on;

    if (strip_update_pending[index] == 0U)
    {
      continue;
    }

    /* 값을 읽기 전에 플래그부터 내린다. 순서가 반대면 송신하는 동안 ISR이 새로 적은 상태를
     * 플래그와 함께 지워 버려 마지막 명령이 화면에 반영되지 않는다. */
    strip_update_pending[index] = 0U;
    alarm_on = strip_alarm_on[index];

    strip_show(index, alarm_on);
  }
}
#endif /* NEOPIXEL_ENABLED */

/**
 * @brief Channel 한 벌(경광등 + 부저 + 줄 조명)을 켜거나 끈다.
 * @param channel 이 Slave가 담당하는 Channel 번호 (MY_FIRST_CHANNEL ~ MY_LAST_CHANNEL)
 * @param turn_on 0이면 끄고, 그 외에는 켠다
 * @details 채널마다 하드웨어 한 벌이 통째로 따로 있으므로 다른 채널은 건드리지 않는다.
 * 규격의 "경광등과 부저는 같은 채널에서 동시에 ON/OFF" 를 그대로 구현한 자리다.
 *
 * LD2만 예외로 보드 단위다 -- 담당 채널 중 하나라도 켜져 있으면 켠다. 온보드 LED라
 * 채널에 대응시킬 수 없고, "이 보드에서 지금 뭔가 울리는 중"을 한눈에 보는 용도다.
 */
static void apply_channel_state(uint8_t channel, uint8_t turn_on)
{
  const GPIO_PinState level = (turn_on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET;
  const uint8_t index = channel_output_index(channel);
  uint8_t scan;
  uint8_t any_on = 0U;

  if (index == CHANNEL_OUTPUT_NONE)
  {
    return;   /* 호출자가 이미 걸러내지만, 이 함수만 봐도 안전하도록 한 번 더 막는다 */
  }

  HAL_GPIO_WritePin(channel_output[index].relay_port, channel_output[index].relay_pin, level);
  channel_on[index] = (turn_on != 0U) ? 1U : 0U;

#if BUZZER_ENABLED
  /* 부저는 자기 채널만 본다. 옆 채널이 켜져 있어도 이 부저는 조용하다. */
  HAL_GPIO_WritePin(channel_output[index].buzzer_port, channel_output[index].buzzer_pin,
                    (turn_on != 0U) ? BUZZER_LEVEL_ON : BUZZER_LEVEL_OFF);
#endif

#if NEOPIXEL_ENABLED
  /* 줄 조명도 채널별이다. 여기서는 원하는 상태만 적어 둔다 -- 실제 송신은 블로킹이라
   * ISR에서 할 수 없다. */
  strip_alarm_on[index] = (uint8_t)((turn_on != 0U) ? 1U : 0U);
  strip_update_pending[index] = 1U;
#endif

  for (scan = 0U; scan < (uint8_t)CHANNEL_OUTPUT_COUNT; ++scan)
  {
    if (channel_on[scan] != 0U)
    {
      any_on = 1U;
    }
  }
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, (any_on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/**
 * @brief Master가 보낸 "S<슬레이브>:CH<채널>:<ON|OFF>" 프레임 한 줄을 해석한다.
 * @param line 개행을 제외한 프레임 본문 (NUL로 끝나지 않음)
 * @param length line의 길이
 * @details RS-485는 버스를 공유하므로 다른 Slave에게 가는 프레임도 그대로 들어온다.
 * 프레임 앞의 슬레이브 번호가 MY_SLAVE_ID와 다르면 버린다.
 * 형식이 어긋난 줄도 아무 동작 없이 버린다.
 * @retval VERDICT_* 어디서 버렸는지(또는 수용했는지). SLAVE_DEBUG_LOG용 진단 값이다.
 */
static uint8_t process_rs485_line(const char *line, uint8_t length)
{
  uint8_t channel;
  uint8_t turn_on;

  /* 고정 위치 검사: "S1:CH1:ON"(9자) 또는 "S1:CH1:OFF"(10자) */
  if (length < 9U || line[0] != 'S' || line[2] != ':' ||
      line[3] != 'C' || line[4] != 'H' || line[6] != ':')
  {
    return VERDICT_BAD_FORMAT;
  }
  if (line[1] < '1' || line[1] > '9' || line[5] < '1' || line[5] > '9')
  {
    return VERDICT_BAD_FORMAT;
  }

  /* 내 주소가 아니면 다른 Slave의 명령이다. */
  if ((uint8_t)(line[1] - '0') != MY_SLAVE_ID)
  {
    return VERDICT_OTHER_SLAVE;
  }

  if (length == 9U && line[7] == 'O' && line[8] == 'N')
  {
    turn_on = 1U;
  }
  else if (length == 10U && line[7] == 'O' && line[8] == 'F' && line[9] == 'F')
  {
    turn_on = 0U;
  }
  else
  {
    return VERDICT_BAD_ACTION;
  }

  /* 출력 표에 없는 채널은 이 보드가 구동하지 않는다. 담당 범위 밖이거나, 범위 안이어도
   * 배선하지 않아 표에서 뺀 채널이 여기 걸린다. 판단 근거는 표 하나뿐이다. */
  channel = (uint8_t)(line[5] - '0');
  if (channel_output_index(channel) == CHANNEL_OUTPUT_NONE)
  {
    return VERDICT_OTHER_CHAN;
  }

  apply_channel_state(channel, turn_on);

#if SLAVE_ACK_ENABLED
  /* GPIO를 실제로 쓴 경우에만 ACK를 예약한다. 버려진 프레임에 ACK를 보내면 Master가
   * 적용되지 않은 명령을 적용된 것으로 기록하게 되어, 이 기능의 목적이 통째로 사라진다. */
  if (ack_pending == 0U)
  {
    ack_channel = channel;
    ack_turn_on = (uint8_t)((turn_on != 0U) ? 1U : 0U);
    ack_pending = 1U;
  }
#endif

  return VERDICT_APPLIED;
}

/**
 * @brief 수신한 한 바이트를 프레임으로 조립하고, 개행을 만나면 해석한다.
 * @details GPIO 제어는 매우 빠르므로 ISR 문맥에서 바로 처리해 반응 지연을 없앤다.
 */
static void process_rs485_byte(uint8_t byte)
{
  if (byte == '\r' || byte == '\n')
  {
    if (rs485_line_overflow == 0U && rs485_line_length != 0U)
    {
      const uint8_t verdict = process_rs485_line(rs485_line, rs485_line_length);
      (void)verdict;
#if SLAVE_DEBUG_LOG
      /* 태스크가 아직 앞 줄을 출력하지 못했으면 이번 줄은 로그만 건너뛴다.
       * GPIO 처리는 위에서 이미 끝났으므로 동작에는 영향이 없다. */
      if (debug_ready == 0U)
      {
        (void)memcpy(debug_line, rs485_line, rs485_line_length);
        debug_line[rs485_line_length] = '\0';
        debug_verdict = verdict;
        debug_ready = 1U;
      }
#endif
    }
    rs485_line_length = 0U;
    rs485_line_overflow = 0U;
    return;
  }

  if (rs485_line_length >= RS485_LINE_BUFFER_SIZE)
  {
    /* 버퍼를 넘긴 줄은 개행까지 통째로 버려 다음 프레임과 섞이지 않게 한다. */
    rs485_line_overflow = 1U;
    return;
  }

  rs485_line[rs485_line_length] = (char)byte;
  ++rs485_line_length;
}

/**
 * @brief USART1 수신 완료 인터럽트. 한 바이트를 처리하고 즉시 재무장한다.
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    process_rs485_byte(rs485_rx_byte);
    rs485_last_rx_msec = HAL_GetTick();
    (void)HAL_UART_Receive_IT(huart, &rs485_rx_byte, 1U);
  }
}

/**
 * @brief USART1 오류(오버런/프레이밍 등) 처리. 조립 중이던 프레임을 버리고 재무장한다.
 * @details 재무장하지 않으면 오류 한 번으로 수신이 영구히 멈춘다.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    rs485_line_length = 0U;
    rs485_line_overflow = 0U;
    (void)HAL_UART_Receive_IT(huart, &rs485_rx_byte, 1U);
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
  MX_USART2_UART_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  /* 담당 채널의 릴레이와 부저를 전부 꺼진 상태로 잡는다. 표를 돌며 초기화하므로
   * 채널을 늘리거나 핀을 옮겨도 이 호출은 그대로다. 스케줄러가 뜨기 전에 끝내야
   * 부팅 중에 경광등이 붙거나 부저가 울리지 않는다. */
  channel_hardware_init();
  /* 줄 조명(NeoPixel)은 여기서 잡지 않는다. neopixel_init()이 HAL_Delay()를 쓰고
   * 첫 송신도 곧바로 이어져야 해서, 태스크 문맥인 StartDefaultTask에서 초기화한다. */
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
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
  HAL_GPIO_WritePin(GPIOA, RELAY_A_Pin|LD2_Pin, GPIO_PIN_RESET);

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

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
#if SLAVE_ACK_ENABLED
/**
 * @brief RS-485 트랜시버의 송신 드라이버를 켤 수 있게 DE 핀을 출력으로 잡는다.
 * @details RS485_DE_ENABLED가 0이면 아무것도 하지 않는다(자동 방향 전환 트랜시버).
 * 기본 상태는 수신(DE=Low)이다 -- 버스를 놓아두어야 Master의 명령을 받을 수 있다.
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
 * @brief 적용한 명령을 그대로 되읽어 Master로 ACK를 보낸다.
 * @param channel 적용한 Channel 번호 (1-based, 회선 위 표현과 같다)
 * @param turn_on 0이면 OFF, 그 외에는 ON
 * @details 태스크 문맥에서만 부를 것 -- 블로킹 송신이다.
 *
 * DE 제어가 켜져 있으면 송신 전후로 드라이버를 열고 닫는다. HAL_UART_Transmit()은
 * 마지막 바이트의 TC(전송 완료) 플래그까지 기다린 뒤 반환하므로, 반환 직후 DE를 내려도
 * 마지막 비트가 잘리지 않는다.
 */
static void rs485_send_ack(uint8_t channel, uint8_t turn_on)
{
  char frame[RS485_ACK_BUFFER_SIZE];
  uint8_t length = 0U;

  frame[length++] = 'A';
  frame[length++] = (char)('0' + MY_SLAVE_ID);
  frame[length++] = ':';
  frame[length++] = 'C';
  frame[length++] = 'H';
  frame[length++] = (char)('0' + channel);
  frame[length++] = ':';
  frame[length++] = 'O';
  if (turn_on != 0U)
  {
    frame[length++] = 'N';
  }
  else
  {
    frame[length++] = 'F';
    frame[length++] = 'F';
  }
  frame[length++] = '\n';

#if RS485_DE_ENABLED
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_SET);
#endif

  (void)HAL_UART_Transmit(&huart1, (const uint8_t*)frame, length, RS485_ACK_TX_TIMEOUT_MSEC);

#if RS485_DE_ENABLED
  HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
#endif
}
#endif /* SLAVE_ACK_ENABLED */
/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN 5 */
  /* 이 보드가 몇 번으로 구워졌는지 부팅 때 스스로 밝힌다. LD2를 MY_SLAVE_ID번 깜빡이므로
   * USB를 연결하지 않아도 보드만 보고 번호를 셀 수 있다. 릴레이(PA0)는 건드리지 않는다. */
  for (uint8_t blink = 0U; blink < MY_SLAVE_ID; ++blink)
  {
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
    osDelay(SLAVE_ID_BLINK_MSEC);
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);
    osDelay(SLAVE_ID_BLINK_MSEC);
  }

#if SLAVE_DEBUG_LOG
  /* 담당 채널과 그 출력 핀을 표에서 그대로 뽑아 찍는다. 채널 수를 늘려도 이 배너는
   * 손댈 필요가 없고, 배선이 어긋났을 때 보드가 스스로 뭘 담당하는지 밝혀 준다. */
  slave_print("\r\nSLAVE #");
  slave_print_digit(MY_SLAVE_ID);
  slave_print(" READY - owns ");
  for (uint8_t index = 0U; index < (uint8_t)CHANNEL_OUTPUT_COUNT; ++index)
  {
    if (index != 0U)
    {
      slave_print(", ");
    }
    slave_print("CH");
    slave_print_digit(channel_output[index].channel);
    slave_print("=");
    slave_print(channel_output[index].set_name);
    slave_print("(");
    slave_print(channel_output[index].wiring);
    slave_print(")");

    /* 표의 채널이 이 보드의 담당 범위를 벗어나면 다른 보드에게 갈 명령에도 반응하게 된다.
     * 조용히 두면 "경광등 두 개가 같이 울린다" 로만 드러나 원인 찾기가 어려우므로,
     * 부팅 배너에서 바로 밝힌다. */
    if ((channel_output[index].channel < MY_FIRST_CHANNEL) ||
        (channel_output[index].channel > MY_LAST_CHANNEL))
    {
      slave_print("!!OUT-OF-RANGE");
    }
  }
#if BUZZER_ENABLED
  /* 극성까지 찍는다. 부팅하자마자 계속 울리면 배선이 아니라 이 설정이 뒤집힌 것이다. */
  slave_print(", buzzer=");
  slave_print(BUZZER_ACTIVE_HIGH ? "active-high" : "active-low");
#endif
#if NEOPIXEL_ENABLED
  /* 줄 조명이 몇 개짜리로 구워졌는지 밝힌다. 줄 끝이 어두우면 이 숫자부터 의심할 것. */
  slave_print(", strip x");
  slave_print_digit((uint8_t)((NEOPIXEL_PIXEL_COUNT / 100U) % 10U));
  slave_print_digit((uint8_t)((NEOPIXEL_PIXEL_COUNT / 10U) % 10U));
  slave_print_digit((uint8_t)(NEOPIXEL_PIXEL_COUNT % 10U));
#endif
  slave_print("\r\n");
#endif

#if NEOPIXEL_ENABLED
  /* 담당 채널의 줄 조명을 전부 안전(초록)으로 켜 두고 시작한다. 명령이 오기 전에도 색이
   * 들어오므로, 이것만 보고 "보드가 살아 있고 스트립 배선도 맞다"를 알 수 있다.
   * 수신을 열기 전에 끝낸다. 첫 프레임 처리와 9ms짜리 송신이 겹치지 않게 하기 위해서다. */
  for (uint8_t index = 0U; index < (uint8_t)CHANNEL_OUTPUT_COUNT; ++index)
  {
    neopixel_init(channel_output[index].strip);
    strip_show(index, 0U);
  }
#endif

#if SLAVE_ACK_ENABLED
  /* 수신 상태(DE=Low)로 만들어 두고 시작한다. 수신을 열기 전에 불러야 첫 명령을 놓치지 않는다. */
  rs485_de_init();
#endif

  /* 인터럽트 수신 시작. 이후 재무장은 각 콜백이 담당한다. */
  (void)HAL_UART_Receive_IT(&huart1, &rs485_rx_byte, 1U);

  /* Infinite loop */
  for(;;)
  {
#if SLAVE_ACK_ENABLED
    /* ISR이 적용을 끝낸 명령에 대해 Master로 ACK를 되보낸다. 디버그 출력보다 먼저 처리해
     * 왕복 지연을 줄인다 -- Master는 이 응답을 타임아웃 안에 받아야 적용을 확인할 수 있다. */
    if (ack_pending != 0U)
    {
      const uint8_t channel = ack_channel;
      const uint8_t turn_on = ack_turn_on;

      rs485_send_ack(channel, turn_on);
      ack_pending = 0U;
    }
#endif

#if SLAVE_DEBUG_LOG
    /* ISR이 넘긴 수신 줄과 판정을 출력한다. 프레임이 아예 안 오는 것인지,
     * 와서 버려지는 것인지, 받아서 GPIO까지 쓴 것인지가 이 한 줄로 갈린다. */
    if (debug_ready != 0U)
    {
      slave_print("RX \"");
      slave_print(debug_line);
      slave_print("\" -> ");
      slave_print(verdict_text[debug_verdict]);
      slave_print("\r\n");
      debug_ready = 0U;
    }
#endif

#if NEOPIXEL_ENABLED
    /* ISR이 예약한 색을 실제로 줄 조명에 내보낸다. ACK보다 뒤에 두었다 -- 300픽셀 송신이
     * 약 9ms 걸려서, 먼저 하면 Master가 기다리는 ACK가 그만큼 늦어진다. */
    neopixel_service();
#endif

    /* 프레임이 끊긴 채 남아 있으면 폐기해 다음 프레임과 섞이지 않게 한다. */
    if ((rs485_line_length != 0U || rs485_line_overflow != 0U) &&
        (HAL_GetTick() - rs485_last_rx_msec) >= RS485_IDLE_RESET_MSEC)
    {
      rs485_line_length = 0U;
      rs485_line_overflow = 0U;
    }

    osDelay(10);
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
