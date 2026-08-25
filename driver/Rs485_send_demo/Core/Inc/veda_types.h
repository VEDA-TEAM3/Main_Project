/**
 * @file veda_types.h
 * @brief Master 안에서 모듈들이 나눠 쓰는 자료형. 규약 구조체의 크기 검사도 여기서 한다.
 *
 * 여기 있는 타입은 전부 "여러 모듈이 함께 보는 것"만 남긴다. 한 모듈 안에서만 쓰는 타입은
 * 그 모듈의 헤더에 둔다(예: 하행 상태머신의 downlink_state_t 는 veda_downlink.h).
 *
 * _Static_assert 를 이 헤더에 둔 이유: 포함하는 모든 컴파일 단위에서 매번 검사되므로,
 * 어느 모듈이 규약 헤더를 다른 정렬 설정으로 끌어와도 그 자리에서 빌드가 멈춘다.
 */

#ifndef VEDA_TYPES_H
#define VEDA_TYPES_H

#include <stdint.h>

#include "driver_protocol.h"

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
 *
 * led_* 세 개는 물리 LED가 아니라 '표시 중인 위험 등급'의 인코딩이다. RPi의
 * decodeRiskLevel() 이 red->Danger, yellow->Warning, 그 외->None 으로 되읽으므로
 * 이 규약을 그대로 지켜야 한다. 셋 다 0인 조합은 '아직 확인 전'을 뜻한다
 * (veda_channel.h 의 channel_boot_state_init 설명 참고).
 */
typedef struct
{
  uint8_t risk_level;   /**< 마지막으로 적용한 veda_risk_level_t */
  uint8_t siren_on;     /**< PA0 경광등 지령 상태. 이 시스템의 유일한 액추에이터다. */
  uint8_t buzzer_on;    /**< Master 보드의 부저는 배선된 적이 없다. 항상 0을 올린다. */
  uint8_t led_red;      /**< 아래 3개는 물리 LED가 아니라 '표시 중인 위험 등급'의 인코딩이다. */
  uint8_t led_yellow;   /**< RPi의 decodeRiskLevel()이 red->Danger, yellow->Warning, */
  uint8_t led_green;    /**< 그 외->None 으로 되읽으므로 이 규약을 그대로 지켜야 한다. */
} channel_status_t;

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

/**
 * @brief Slave가 되보낸 ACK 한 장. "A<슬레이브>:CH<채널>:R<위험도>" 를 파싱한 결과다.
 * @details ISR(ack_parse.inc)이 만들어 큐에 넣고 sched_task(ack_match.inc)가 꺼내 대조한다.
 * 회선 위 표현이 그대로 들어 있으므로 두 숫자 모두 1-based 다.
 */
typedef struct
{
  uint8_t slave;      /**< 1-based 슬레이브 번호 */
  uint8_t channel;    /**< 1-based 채널 번호 (RS-485 회선 위 표현) */
  uint8_t risk_level; /**< Slave가 실제로 적용한 veda_risk_level_t (0/1/2) */
} rs485_ack_t;

#endif /* VEDA_TYPES_H */
