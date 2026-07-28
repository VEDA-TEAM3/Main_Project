/**
 * @file    app_tasks.c
 * @brief   Rpi <-> STM32 제어 파이프라인 태스크 구현 (VEDA 바이너리 프레임 프로토콜)
 *
 * [전체 흐름 요약]
 * 1) USART1 RX 인터럽트가 1바이트씩 들어올 때마다 main.c의
 *    HAL_UART_RxCpltCallback -> App_UartRxByteFromISR()를 거쳐
 *    rawRxByteQueue에 그 바이트 1개만 담긴다.
 * 2) rx_task가 rawRxByteQueue를 계속 비우면서 driver_protocol.h의
 *    veda_downlink_frame_t 포맷(START, payload 24B, checksum, END)을 바이트 단위로
 *    조립/검증한다. 체크섬까지 맞는 완전한 프레임이 완성되면 veda_risk_event_t를
 *    cmdQueue에 넣는다. START/END가 어긋나거나 체크섬이 틀리면 그 프레임은 버리고
 *    다음 바이트부터 새 프레임 탐색을 재시작한다(resync).
 * 3) control_task가 cmdQueue에서 risk_event를 꺼내 channelRiskLevel[channel_id]를
 *    갱신하고, 현재 GPIO 상태를 읽어 veda_uplink_packet_t(ACK)로 만들어 feedbackQueue에
 *    넣는다. 실제 LED GPIO 쓰기는 이 태스크가 아니라 led_task가 담당한다(아래 7번).
 * 4) schedule_task는 HEARTBEAT_PERIOD_MS(500ms)마다 heartbeatTriggerSem을 풀어(release)
 *    heartbeat_task를 깨운다.
 * 5) heartbeat_task는 세마포어를 받을 때마다 CH0~CH3 각각에 대해 siren/buzzer GPIO 상태와
 *    channelRiskLevel을 읽어 veda_uplink_packet_t(HEARTBEAT) 4개를 만들어 feedbackQueue에
 *    넣는다. (이 보드 한 대가 4채널 LED를 모두 구동하므로 4채널 전부를 보고해야
 *    RPi watchdog이 나머지 채널을 dead로 오판하지 않는다)
 * 6) tx_task는 feedbackQueue 하나만 바라보고 있다가, control_task든 heartbeat_task든
 *    누가 넣었든 상관없이 순서대로 꺼내 veda_uplink_frame_t로 프레이밍해서
 *    USART1로 Rpi에 전송한다. ACK와 HEARTBEAT는 payload.reason 필드로 구분되므로
 *    Rpi 쪽에서 문자열 접두어 비교 없이 바로 분기할 수 있다.
 * 7) led_task는 LED_UPDATE_PERIOD_MS(300ms)마다 주기 실행되어 channelRiskLevel[]을 읽고
 *    channelLed[] 매핑(CH0=PB6 노랑, CH1=PB2 초록, CH2=PB1 노랑, CH3=PB15 빨강)에
 *    따라 4채널 LED를 모두 갱신한다.
 *    DANGER=계속 켜짐, WARNING=매 주기 토글(깜빡임), NONE(safe)=꺼짐.
 *    control_task와 별도 주기 태스크로 분리한 이유는, 이벤트가 새로 안 들어와도
 *    WARNING 상태의 깜빡임이 계속 유지되어야 하기 때문이다.
 */
#include "app_tasks.h"
#include <string.h>
#include <stdio.h>

extern UART_HandleTypeDef huart1;   /* Rpi 링크 (PA9/PA10), main.c에서 정의됨 */

/* ---- Queue / Semaphore handles ----
 * rawRxByteQueueHandle : ISR -> rx_task. 원소 1개 = 수신 바이트 1개(uint8_t)
 * cmdQueueHandle       : rx_task -> control_task. 원소 1개 = 검증된 risk_event(veda_risk_event_t)
 * feedbackQueueHandle  : control_task/heartbeat_task -> tx_task. 원소 1개 = veda_uplink_packet_t
 * heartbeatTriggerSemHandle : schedule_task -> heartbeat_task를 깨우는 binary semaphore
 */
static osMessageQueueId_t rawRxByteQueueHandle;
static osMessageQueueId_t cmdQueueHandle;
static osMessageQueueId_t feedbackQueueHandle;
static osSemaphoreId_t heartbeatTriggerSemHandle;

/* ---- Task handles ---- */
static osThreadId_t rxTaskHandle;
static osThreadId_t controlTaskHandle;
static osThreadId_t txTaskHandle;
static osThreadId_t heartbeatTaskHandle;
static osThreadId_t scheduleTaskHandle;
static osThreadId_t ledTaskHandle;

static const osThreadAttr_t rxTask_attributes = {
    .name = "rx_task",
    .stack_size = 256 * 4,
    .priority = (osPriority_t) osPriorityHigh,
};
static const osThreadAttr_t controlTask_attributes = {
    .name = "control_task",
    .stack_size = 256 * 4,
    .priority = (osPriority_t) osPriorityAboveNormal,
};
static const osThreadAttr_t txTask_attributes = {
    .name = "tx_task",
    .stack_size = 256 * 4,
    .priority = (osPriority_t) osPriorityAboveNormal,
};
static const osThreadAttr_t scheduleTask_attributes = {
    .name = "schedule_task",
    /* 원래 128*4(configMINIMAL_STACK_SIZE, 여유 0)였는데, task 성능 측정용 printf()를
     * 추가하면서 스택이 부족해졌다. configCHECK_FOR_STACK_OVERFLOW가 꺼져 있어 오버플로우가
     * 감지되지 않고 조용히 인접 메모리를 덮어써서(led_task 등이 갑자기 멈추는 등) 원인 파악이
     * 어려우므로, printf를 쓰는 다른 태스크(rx/control/tx_task)와 같은 256*4로 맞춘다. */
    .stack_size = 256 * 4,
    .priority = (osPriority_t) osPriorityNormal,
};
static const osThreadAttr_t heartbeatTask_attributes = {
    .name = "heartbeat_task",
    .stack_size = 256 * 4,
    .priority = (osPriority_t) osPriorityLow,
};
static const osThreadAttr_t ledTask_attributes = {
    .name = "led_task",
    .stack_size = 256 * 4,
    .priority = (osPriority_t) osPriorityLow,
};

/* 태스크별 처리시간 프로파일링 로그 스위치 (기본 꺼짐).
 *
 * printf() 는 _write() -> HAL_UART_Transmit(&huart2, ..., HAL_MAX_DELAY) 로 블로킹
 * 전송된다. 115200 기준 문자당 ~87us 라 30자 한 줄이면 ~2.6ms 를 통째로 잡아먹고,
 * 그 사이 FreeRTOS 크리티컬 섹션이 USART1(RPi 링크) RX 인터럽트를 반복해서 마스킹한다.
 * STM32F4 USART 에는 RX FIFO 가 없어 86.8us 만 밀려도 오버런(ORE)이 나고, 오버런이 나면
 * HAL 이 RX 인터럽트를 꺼버린다(main.c 의 HAL_UART_ErrorCallback 주석 참고).
 *
 * -> 프레임마다/300ms 마다 도는 핫패스에서 이 로그를 켜두면 RPi 링크 수신이 깨진다.
 *    처리시간을 실제로 재야 할 때만 1 로 바꿔서 쓰고, 측정이 끝나면 다시 0 으로 되돌릴 것.
 */
#define APP_TASK_PROFILING_LOG 0

#if APP_TASK_PROFILING_LOG
#define APP_PROF_PRINTF(...) printf(__VA_ARGS__)
#else
/* 꺼져 있어도 컴파일러가 인자를 '사용된 것'으로 보게 해서 -Wall 의 unused 경고를 막고,
 * 포맷 문자열 타입 검사도 계속 받게 한다. if(0) 블록은 상수 조건이라 코드가 남지 않는다. */
#define APP_PROF_PRINTF(...)      \
  do {                            \
    if (0) printf(__VA_ARGS__);   \
  } while (0)
#endif

/* STM32 -> Rpi heartbeat 주기 (configTICK_RATE_HZ=1000 이라 tick == ms).
 *
 * RPi 쪽 AppConfig 기본값이 heartbeatIntervalMs=500, missedBeatsForTimeout=3 이라
 * watchdog 타임아웃이 1500ms 다. 예전 값(5000ms)은 이 타임아웃보다 길어서, 채널이
 * heartbeat 를 받아 alive 로 올라갔다가 1.5초 뒤 dead 로 떨어지는 걸 5초마다 무한 반복했다
 * (SerialHwEventDispatcher::watchdogLoop). 서버 설정값에 그대로 맞춘다. */
#define HEARTBEAT_PERIOD_MS   500

/* 채널 LED 상태 갱신 주기. WARNING일 때 이 주기로 토글되어 깜빡임으로 보인다. */
#define LED_UPDATE_PERIOD_MS  300

/* rx_task의 프레임 조립 상태머신 단계 */
typedef enum {
    RX_WAIT_START = 0,   /* START_BYTE를 기다림 */
    RX_READ_PAYLOAD,     /* payload(veda_risk_event_t) 바이트를 채우는 중 */
    RX_READ_CHECKSUM,    /* checksum 바이트 1개를 기다림 */
    RX_WAIT_END,         /* END_BYTE를 기다림 */
} RxFrameState_t;

static void rx_task(void *argument);
static void control_task(void *argument);
static void tx_task(void *argument);
static void heartbeat_task(void *argument);
static void schedule_task(void *argument);
static void led_task(void *argument);

static void read_actuator_state(uint8_t channel_id, veda_uplink_packet_t *fb_out);
static void apply_channel_actuators(uint8_t channel_id, veda_risk_level_t level, veda_uplink_packet_t *fb_out);
static void control_process_command(const veda_risk_event_t *cmd, veda_uplink_packet_t *fb_out);
static void heartbeat_build_message(uint8_t channel_id, veda_uplink_packet_t *fb_out);
static void update_channel_led(GPIO_TypeDef *port, uint16_t pin, veda_risk_level_t level, uint8_t *blinkState);

/* Contract.h 기준 CCTV 채널 수(0..3) */
#define MAX_CHANNELS 4

/* 채널별로 마지막에 받은 risk_level을 기억해둔다. control_task(이벤트 수신 시)가 갱신하고,
 * led_task(주기 실행)가 읽어서 CH0~CH3 LED를 갱신하며, heartbeat/ACK 를 만들 때
 * read_actuator_state()도 이 값을 channel_risk_level 필드에 실어 보낸다 — 여러 태스크가
 * 공유하는 상태이지만 각 원소가 1바이트 enum 단일 대입/읽기라 Cortex-M에서 원자적이므로
 * 별도 뮤텍스 없이 사용한다. */
static veda_risk_level_t channelRiskLevel[MAX_CHANNELS];

/**
 * channel_id(프로토콜, 0-index) -> LED GPIO 매핑.
 * CH0(channel_id=0) = PB6  노랑 (CN10-17에 배선)
 * CH1(channel_id=1) = PB2  초록
 * CH2(channel_id=2) = PB1  노랑
 * CH3(channel_id=3) = PB15 빨강
 * 배열 인덱스가 곧 channel_id이므로 channelRiskLevel[channel_id]를 그대로 매칭할 수 있다.
 */
static const struct { GPIO_TypeDef *port; uint16_t pin; } channelLed[MAX_CHANNELS] = {
    { CH0_LED_GPIO_Port, CH0_LED_Pin },  /* channel_id 0 */
    { CH1_LED_GPIO_Port, CH1_LED_Pin },  /* channel_id 1 */
    { CH2_LED_GPIO_Port, CH2_LED_Pin },  /* channel_id 2 */
    { CH3_LED_GPIO_Port, CH3_LED_Pin },  /* channel_id 3 */
};

/* WARNING 깜빡임의 채널별 토글 위상. led_task(주기)와 control_task(명령 수신 즉시)가
 * 함께 갱신하지만, 1바이트 대입/읽기라 Cortex-M에서 원자적이다.
 *
 * 초기값: 모든 채널이 led_task의 같은 for문 안에서 같은 tick에 토글되므로, 초기값이
 * 같으면 WARNING 채널끼리 위상이 항상 같이 움직인다(동시에 켜지고 동시에 꺼짐).
 * CH2(index 2)만 1로 시작시켜 CH1(index 1)과 매 tick 반대로 켜지고 꺼지게 만든다. */
static uint8_t ledBlinkState[MAX_CHANNELS] = {0, 0, 1, 0};

/**
 * App_TasksInit: main()의 osKernelInitialize() 이후, osKernelStart() 이전에 딱 한 번 호출된다.
 */
void App_TasksInit(void)
{
    rawRxByteQueueHandle      = osMessageQueueNew(64, sizeof(uint8_t), NULL);
    cmdQueueHandle            = osMessageQueueNew(8, sizeof(veda_risk_event_t), NULL);
    feedbackQueueHandle       = osMessageQueueNew(8, sizeof(veda_uplink_packet_t), NULL);
    heartbeatTriggerSemHandle = osSemaphoreNew(1, 0, NULL);

    rxTaskHandle        = osThreadNew(rx_task, NULL, &rxTask_attributes);
    controlTaskHandle   = osThreadNew(control_task, NULL, &controlTask_attributes);
    txTaskHandle        = osThreadNew(tx_task, NULL, &txTask_attributes);
    scheduleTaskHandle  = osThreadNew(schedule_task, NULL, &scheduleTask_attributes);
    heartbeatTaskHandle = osThreadNew(heartbeat_task, NULL, &heartbeatTask_attributes);
    ledTaskHandle       = osThreadNew(led_task, NULL, &ledTask_attributes);

    /* configTOTAL_HEAP_SIZE(15360B)가 부족하면 osThreadNew/osMessageQueueNew 는 조용히
     * NULL 을 돌려준다. 그대로 두면 led_task 가 안 만들어져도 부팅은 되고 LED 만 영영
     * 안 켜지는 식으로 증상이 엉뚱하게 나타나므로, 여기서 즉시 Error_Handler()로 잡는다. */
    if (rawRxByteQueueHandle == NULL || cmdQueueHandle == NULL || feedbackQueueHandle == NULL ||
        heartbeatTriggerSemHandle == NULL ||
        rxTaskHandle == NULL || controlTaskHandle == NULL || txTaskHandle == NULL ||
        scheduleTaskHandle == NULL || heartbeatTaskHandle == NULL || ledTaskHandle == NULL)
    {
        Error_Handler();
    }
}

/**
 * App_UartRxByteFromISR: HAL_UART_RxCpltCallback(ISR 컨텍스트)에서 매 바이트마다 호출됨.
 * ISR 안에서는 절대 오래 블로킹하면 안 되므로 timeout=0으로 큐에 넣고,
 * 큐가 가득 차 있으면 그냥 실패(byte 유실)하고 즉시 리턴한다.
 */
void App_UartRxByteFromISR(uint8_t byte)
{
    /* 큐 생성 전(또는 생성 실패)에 인터럽트가 뜬 경우 방어. 정상 순서라면 rx_task 가
     * App_UartRxArm() 을 부른 뒤부터 인터럽트가 오므로 여기서 걸릴 일은 없다. */
    if (rawRxByteQueueHandle == NULL)
    {
        return;
    }
    /* TODO: overflow 시 카운터/에러 플래그 등으로 가시화할지 결정 필요 */
    osMessageQueuePut(rawRxByteQueueHandle, &byte, 0, 0);
}

/**
 * rx_task: raw byte queue를 읽어 veda_downlink_frame_t(START, payload 24B, checksum, END)를
 * 바이트 단위로 조립/검증한다. 체크섬이 안 맞거나 END_BYTE가 어긋나면 그 프레임은 조용히
 * 버리고 RX_WAIT_START로 되돌아가 다음 바이트부터 새 프레임을 다시 찾는다.
 */
static void rx_task(void *argument)
{
    uint8_t byte;
    RxFrameState_t state = RX_WAIT_START;
    uint8_t payloadBuf[sizeof(veda_risk_event_t)];
    uint8_t payloadIdx = 0;
    uint8_t rxChecksum = 0;
    /* task 성능 측정: 프레임 조립 소요시간(완료시간 - 시작시간).
     * 시작 = START_BYTE를 찾은 시점, 완료 = 체크섬/END_BYTE까지 검증되어
     * 완전한 프레임 하나가 완성된 시점. */
    uint32_t frameStartTick = 0;

    /* 큐가 준비된 뒤에 USART1 수신을 연다(main() 이 아니라 여기인 이유는 main.c 주석 참고) */
    App_UartRxArm();

    for (;;)
    {
        if (osMessageQueueGet(rawRxByteQueueHandle, &byte, NULL, osWaitForever) != osOK)
        {
            continue;
        }

        switch (state)
        {
        case RX_WAIT_START:
            if (byte == VEDA_START_BYTE)
            {
                payloadIdx = 0;
                frameStartTick = HAL_GetTick();
                state = RX_READ_PAYLOAD;
            }
            break;

        case RX_READ_PAYLOAD:
            payloadBuf[payloadIdx++] = byte;
            if (payloadIdx == sizeof(payloadBuf))
            {
                state = RX_READ_CHECKSUM;
            }
            break;

        case RX_READ_CHECKSUM:
            rxChecksum = byte;
            state = RX_WAIT_END;
            break;

        case RX_WAIT_END:
            if (byte == VEDA_END_BYTE && veda_checksum(payloadBuf, sizeof(payloadBuf)) == rxChecksum)
            {
                veda_risk_event_t cmd;
                memcpy(&cmd, payloadBuf, sizeof(cmd));
                /* TODO: Command Queue full 시 정책(드롭/에러 피드백) 결정 필요 */
                osMessageQueuePut(cmdQueueHandle, &cmd, 0, 0);

                APP_PROF_PRINTF("[rx_task] frame assembled in %lums\r\n",
                                (unsigned long)(HAL_GetTick() - frameStartTick));
            }
            state = RX_WAIT_START;
            break;
        }
    }
}

/**
 * control_task: Command Queue -> HW 제어 -> Feedback Queue
 */
static void control_task(void *argument)
{
    veda_risk_event_t cmd;
    veda_uplink_packet_t fb;

    for (;;)
    {
        if (osMessageQueueGet(cmdQueueHandle, &cmd, NULL, osWaitForever) == osOK)
        {
            /* 서버(RPi)가 보낸 risk_event를 실제로 받았는지 눈으로 확인하기 위한 디버그 로그.
             * USART2(ST-Link VCP)로 나가므로 STM32CubeIDE 시리얼 모니터에서 확인.
             * timestamp_ms는 64bit라 %lu로 하위 32bit만 잘려서 찍힘(디버그 용도라 무방) */
            APP_PROF_PRINTF("[control_task] RX risk_event: ch=%u level=%u dist_mm=%u ts=%lu\r\n",
                            (unsigned)cmd.channel_id, (unsigned)cmd.risk_level, (unsigned)cmd.dist_mm,
                            (unsigned long)cmd.timestamp_ms);

            /* task 성능 측정: 명령 처리 소요시간(완료시간 - 시작시간).
             * 시작 = cmdQueue에서 명령을 꺼낸 시점, 완료 = ACK를 만들어 feedbackQueue에
             * 넣기 직전. HAL_GetTick()은 1ms 분해능이라 매우 짧은 처리는 0ms로 찍힐 수 있음. */
            uint32_t startTick = HAL_GetTick();

            control_process_command(&cmd, &fb);

            uint32_t elapsedMs = HAL_GetTick() - startTick;
            APP_PROF_PRINTF("[control_task] processed in %lums\r\n", (unsigned long)elapsedMs);

            /* TODO: Feedback Queue full 시 정책 결정 필요 */
            osMessageQueuePut(feedbackQueueHandle, &fb, 0, 0);
        }
    }
}

/**
 * 이 채널의 상태를 상행 패킷에 채운다. apply_channel_actuators()의 ACK 보고와
 * heartbeat_build_message() 양쪽에서 공통으로 쓴다.
 *
 * [led_red / led_yellow / led_green 을 GPIO 에서 읽지 않는 이유]
 * RPi 의 SerialHwEventDispatcher::decodeRiskLevel() 은 이 세 필드로 "이 채널이 지금
 * 표시 중인 risk level"을 복원해서 자기가 마지막으로 보낸 명령과 비교한다
 * (checkChannelMismatch). 즉 이 필드들은 '전구가 켜졌나'가 아니라 '레벨이 뭔가'를 나르는
 * 자리다. GPIO 를 그대로 읽으면 두 가지 이유로 반드시 불일치가 난다:
 *
 *  1) WARNING 은 LED_UPDATE_PERIOD_MS 주기 깜빡임이라, GPIO 를 읽는 순간의 위상이 0/1
 *     랜덤이다. HEARTBEAT 마다 절반의 확률로 '꺼져 있음'으로 보고되고, RPi 는 이걸 명령
 *     불이행으로 보고 재전송하다 재시도를 소진해 fault 로 에스컬레이션한다. 그 뒤로는
 *     dispatch()가 '변경분만 전송'하므로 같은 레벨이 다시 와도 명령을 안 보내서,
 *     LED 가 영영 안 켜지는 상태로 굳는다.
 *  2) 이 보드는 채널당 LED 가 1개이고 색도 채널마다 다르다(CH0 노랑, CH1 초록, CH2 노랑,
 *     CH3 빨강). 채널당 3색 신호등이 아니라서 물리적인 색과 risk level 사이에 대응이 없다.
 *     CH0 에 DANGER 를 걸면 실제로 켜지는 건 노란 LED 이므로, 색을 그대로 보고하면 RPi 는
 *     Warning 으로 읽는다.
 *
 * -> 그래서 세 필드를 channelRiskLevel[] 에서 파생시킨다. RPi 의 신호등 관례
 *    (red->Danger, yellow->Warning, 그 외 None)와 정확히 대칭이라 decodeRiskLevel()이
 *    보낸 명령을 그대로 되돌려주고, 깜빡임 위상에도 흔들리지 않는다.
 *
 * siren/buzzer 는 단순 on/off 출력이라 GPIO 를 그대로 읽어도 무방하다.
 * offset 7 의 reserved0 은 호출자가 memset 으로 이미 0 을 채워두었다.
 */
static void read_actuator_state(uint8_t channel_id, veda_uplink_packet_t *fb_out)
{
    const veda_risk_level_t level = (channel_id < MAX_CHANNELS) ? channelRiskLevel[channel_id]
                                                                : VEDA_RISK_NONE;

    fb_out->siren_on   = (HAL_GPIO_ReadPin(SIREN_GPIO_Port, SIREN_Pin) == GPIO_PIN_SET);
    fb_out->buzzer_on  = (HAL_GPIO_ReadPin(BUZZER_GPIO_Port, BUZZER_Pin) == GPIO_PIN_SET);
    fb_out->led_red    = (level == VEDA_RISK_DANGER);
    fb_out->led_yellow = (level == VEDA_RISK_WARNING);
    fb_out->led_green  = (level == VEDA_RISK_NONE);
}

/* channel_id -> 액추에이터 라우팅.
 * 깜빡임 유지를 위해 CH0(PB6)~CH3(PB15) LED의 주기적 갱신은 led_task가 맡지만,
 * 명령을 받은 그 채널만은 여기서 즉시 한 번 반영한다 -- led_task를 기다리면 최대
 * LED_UPDATE_PERIOD_MS(300ms) 동안 명령에 아무 반응이 없는 것처럼 보이기 때문이다. */
static void apply_channel_actuators(uint8_t channel_id, veda_risk_level_t level, veda_uplink_packet_t *fb_out)
{
    if (channel_id < MAX_CHANNELS)
    {
        if (level == VEDA_RISK_WARNING)
        {
            /* WARNING 을 update_channel_led()로 처리하면 blinkState를 '토글'하므로, 하필
             * 켜져 있던 위상에 명령이 도착하면 오히려 꺼진다. 명령에 대한 첫 반응은 항상
             * 점등이어야 하므로 위상을 ON으로 리셋하고, 이후 깜빡임은 led_task가 이어받는다. */
            ledBlinkState[channel_id] = 1;
            HAL_GPIO_WritePin(channelLed[channel_id].port, channelLed[channel_id].pin, GPIO_PIN_SET);
        }
        else
        {
            update_channel_led(channelLed[channel_id].port, channelLed[channel_id].pin,
                               level, &ledBlinkState[channel_id]);
        }
    }
    read_actuator_state(channel_id, fb_out);
}

static void control_process_command(const veda_risk_event_t *cmd, veda_uplink_packet_t *fb_out)
{
    if (cmd->channel_id < MAX_CHANNELS)
    {
        channelRiskLevel[cmd->channel_id] = (veda_risk_level_t)cmd->risk_level;
    }

    memset(fb_out, 0, sizeof(*fb_out));
    fb_out->channel_id = cmd->channel_id;
    fb_out->reason     = VEDA_UPLINK_REASON_ACK;

    apply_channel_actuators(cmd->channel_id, (veda_risk_level_t)cmd->risk_level, fb_out);

    fb_out->timestamp_ms = (int64_t)HAL_GetTick();
}

/**
 * tx_task: Feedback Queue -> veda_uplink_frame_t로 프레이밍 -> Rpi(USART1) 송신.
 * control_task/heartbeat_task가 공유하는 단일 출구. 큐는 FIFO이므로
 * 먼저 넣은 순서대로 여기서 꺼내져 그대로 전송된다.
 */
static void tx_task(void *argument)
{
    veda_uplink_packet_t fb;

    for (;;)
    {
        if (osMessageQueueGet(feedbackQueueHandle, &fb, NULL, osWaitForever) == osOK)
        {
            /* task 성능 측정: 시작 = feedbackQueue에서 꺼낸 시점,
             * 완료 = UART 전송(HAL_UART_Transmit)까지 끝난 시점 */
            uint32_t startTick = HAL_GetTick();

            veda_uplink_frame_t frame;
            frame.start_byte = VEDA_START_BYTE;
            frame.payload    = fb;
            frame.checksum   = veda_uplink_checksum(&fb);
            frame.end_byte   = VEDA_END_BYTE;

            HAL_UART_Transmit(&huart1, (uint8_t *)&frame, sizeof(frame), 100);

            APP_PROF_PRINTF("[tx_task] processed in %lums\r\n", (unsigned long)(HAL_GetTick() - startTick));
        }
    }
}

/**
 * heartbeat_task: schedule_task의 주기 트리거(5초)를 받아 현재 siren/buzzer/LED 상태를
 * 담은 HEARTBEAT 패킷을 만들어 Feedback Queue로 보낸다.
 */
static void heartbeat_task(void *argument)
{
    veda_uplink_packet_t fb;

    for (;;)
    {
        if (osSemaphoreAcquire(heartbeatTriggerSemHandle, osWaitForever) == osOK)
        {
            /* task 성능 측정: 시작 = 세마포어를 받은 시점,
             * 완료 = 전 채널 HEARTBEAT 패킷을 feedbackQueue에 넣은 시점 */
            uint32_t startTick = HAL_GetTick();

            /* 이 보드 한 대가 CH0~CH3 LED를 전부 구동하므로 4채널 모두에 대해 보고한다.
             * 예전에는 channel_id를 1로 고정해 한 개만 보냈는데, RPi의
             * SerialHwEventDispatcher::markAlive()가 pkt.channel_id 기준으로 alive를
             * 갱신하는 탓에 나머지 3채널이 watchdog에서 영원히 dead로 판정됐다. */
            for (uint8_t ch = 0; ch < MAX_CHANNELS; ++ch)
            {
                heartbeat_build_message(ch, &fb);
                osMessageQueuePut(feedbackQueueHandle, &fb, 0, 0);
            }

            APP_PROF_PRINTF("[heartbeat_task] processed in %lums\r\n", (unsigned long)(HAL_GetTick() - startTick));
        }
    }
}

/* 지정한 채널의 HEARTBEAT 패킷 하나를 만든다.
 * led_red/led_yellow/led_green 에 그 채널이 실제로 표시 중인 레벨이 실리므로(read_actuator_state
 * 주석 참고), RPi 는 이 패킷만으로도 alive 판정과 명령-상태 불일치 검증(checkChannelMismatch)을
 * 함께 할 수 있다. */
static void heartbeat_build_message(uint8_t channel_id, veda_uplink_packet_t *fb_out)
{
    memset(fb_out, 0, sizeof(*fb_out));
    fb_out->channel_id = channel_id;
    fb_out->reason     = VEDA_UPLINK_REASON_HEARTBEAT;
    read_actuator_state(channel_id, fb_out);
    fb_out->timestamp_ms = (int64_t)HAL_GetTick();
}

/**
 * schedule_task: heartbeat_task의 주기 트리거 담당. HEARTBEAT_PERIOD_MS(500ms)마다
 * 세마포어를 풀어(release) heartbeat_task를 깨운다.
 */
static void schedule_task(void *argument)
{
    for (;;)
    {
        osDelay(HEARTBEAT_PERIOD_MS);

        /* task 성능 측정: 시작 = osDelay가 끝난 시점, 완료 = 세마포어 release 직후.
         * 하는 일이 release 호출 하나뿐이라 항상 0ms에 가깝게 찍힘 — 다른 태스크와
         * 형식만 맞추기 위해 남겨둠(의미 있는 지표는 아님). */
        uint32_t startTick = HAL_GetTick();

        osSemaphoreRelease(heartbeatTriggerSemHandle);

        APP_PROF_PRINTF("[schedule_task] processed in %lums\r\n", (unsigned long)(HAL_GetTick() - startTick));
    }
}

/**
 * update_channel_led: LED 1개로 danger/warning/safe 3단계를 표현한다.
 * DANGER=계속 켜짐, WARNING=led_task 주기(LED_UPDATE_PERIOD_MS)마다 토글되어 깜빡임,
 * NONE(safe)=꺼짐. *blinkState는 채널별 토글 위상을 유지하기 위한 호출자 소유 상태다.
 */
static void update_channel_led(GPIO_TypeDef *port, uint16_t pin, veda_risk_level_t level, uint8_t *blinkState)
{
    switch (level)
    {
    case VEDA_RISK_DANGER:
        *blinkState = 0;
        HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
        break;
    case VEDA_RISK_WARNING:
        *blinkState = !*blinkState;
        HAL_GPIO_WritePin(port, pin, *blinkState ? GPIO_PIN_SET : GPIO_PIN_RESET);
        break;
    case VEDA_RISK_NONE:
    default:
        *blinkState = 0;
        HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
        break;
    }
}

/**
 * led_task: LED_UPDATE_PERIOD_MS(300ms)마다 깨어나 channelRiskLevel[]을 읽어
 * channelLed[]에 매핑된 4개 채널 LED를 모두 갱신한다.
 * control_task(이벤트 수신 시 1회 갱신)와 달리 이 태스크는 항상 주기 실행되어야
 * WARNING의 깜빡임이 이벤트가 없는 동안에도 계속 유지된다.
 */
static void led_task(void *argument)
{
    for (;;)
    {
        /* task 성능 측정: 시작 = 이번 주기 갱신 시작, 완료 = 4채널 GPIO 갱신 전부 끝난 시점 */
        uint32_t startTick = HAL_GetTick();

        for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        {
            update_channel_led(channelLed[ch].port, channelLed[ch].pin, channelRiskLevel[ch], &ledBlinkState[ch]);
        }

        APP_PROF_PRINTF("[led_task] processed in %lums\r\n", (unsigned long)(HAL_GetTick() - startTick));

        osDelay(LED_UPDATE_PERIOD_MS);
    }
}
