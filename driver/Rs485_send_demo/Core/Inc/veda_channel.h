/**
 * @file veda_channel.h
 * @brief 채널 상태의 저장소와 등급 -> 출력 변환. 하행 의미가 상행 의미로 바뀌는 유일한 지점.
 *
 * 두 배열이 이 시스템의 상태 전부다.
 *
 * | 배열 | 무엇 | 쓰는 곳 | 읽는 곳 |
 * |---|---|---|---|
 * | `channel_status[]` | 지금 표시 중인 상태(상행 payload 원본) | sched_task | hb_task, ctrl_task |
 * | `channel_ctrl[]`   | desired / applied 와 지연 계측 | ctrl_task(desired), sched_task(나머지) | 스케줄러 |
 *
 * 인덱스는 둘 다 0-based 이고 `channel_id - RPI_CHANNEL_ID_BASE` 와 같다.
 *
 * `channel_status_mutex` 로 보호하는 것은 `channel_status[]` 뿐이다. sched_task 가 쓰고
 * hb_task 가 읽어 6바이트 구조체를 통째로 복사하므로 찢어진 값을 막아야 한다.
 * `channel_ctrl[].desired_risk` 는 보호하지 않는다 -- 이유는 veda_types.h 의 channel_ctrl_t
 * 설명에 있다.
 */

#ifndef VEDA_CHANNEL_H
#define VEDA_CHANNEL_H

#include <stdint.h>

#include "cmsis_os.h"
#include "veda_config.h"
#include "veda_types.h"

/** Channel별 표시 상태. 인덱스는 0-based(= channel_id - RPI_CHANNEL_ID_BASE). */
extern channel_status_t channel_status[CHANNEL_COUNT];

/** Channel별 스케줄링 상태. 인덱스는 channel_status[] 와 같다. */
extern channel_ctrl_t channel_ctrl[CHANNEL_COUNT];

/** channel_status 를 sched_task(쓰기)와 hb_task(읽기)가 나눠 쓰므로 보호한다. */
extern osMutexId_t channel_status_mutex;

/**
 * @brief channel_status_mutex 를 만든다. osKernelInitialize() 뒤, 태스크 생성 전에 부를 것.
 * @retval 1 성공, 0 실패 (힙 부족 -- Error_Handler 로 갈 것)
 */
uint8_t veda_channel_mutex_create(void);

/**
 * @brief 위험 등급을 이 채널이 표시할 상태로 변환한다. 하행 -> 상행 의미 변환의 유일한 지점이다.
 * @param risk_level veda_risk_level_t (0 = NONE, 1 = WARNING, 2 = DANGER)
 * @param out 변환 결과를 받을 곳
 * @details led_* 는 물리 LED가 아니라 RPi가 되읽을 위험 등급의 인코딩이다
 * (decodeRiskLevel(): red->Danger, yellow->Warning, 그 외->None).
 * 여기 매핑을 바꾸면 RPi의 명령-상태 불일치 판정이 그대로 틀어지므로 저쪽과 함께 볼 것.
 *
 * WARNING도 경광등을 상시 점등한다(규격의 경광등/부저 정책). 등급 자체는 여기서 뭉개지
 * 않는다 -- out->risk_level 이 그대로 남고, 그 값이 RS-485 프레임에 실려 Slave까지 간다.
 * Slave는 그것으로 줄 조명 색(초록/노랑/빨강)을 가른다.
 *
 * ### 부저는 siren_on 을 따라가지 않는다
 * Slave의 부저는 DANGER 에서만 운다(Slave의 apply_channel_state() 참고). 경광등과 조건이
 * 다르므로 siren_on을 따라가면 안 된다 -- 그러면 WARNING 구간에서 RPi의
 * HwIndicatorState(buzzerOn)가 실물과 어긋나 조용한 부저를 울고 있다고 보고하게 된다.
 *
 * 두 곳이 같은 규칙을 따라야 한다. 정책을 바꾸려면 이 함수와 Slave의 apply_channel_state()를
 * **반드시 함께** 고칠 것:
 *
 *   등급       줄 조명   경광등   부저
 *   --------   -------   ------   ----
 *   NONE       초록      OFF      OFF
 *   WARNING    노랑      ON       OFF
 *   DANGER     빨강      ON       ON
 *
 * ### default 분기를 남겨 두는 이유
 * 규약에 없는 등급은 ctrl_task 가 이 함수에 들어오기 전에 이미 세어 두므로 default 로는
 * 오지 않는다. 그래도 남겨 둔다 -- 다른 호출자가 생겨도 경광등을 켠 채로 두지 않게
 * 안전한 쪽(NONE = 소등)으로 떨어뜨린다.
 */
void risk_to_status(uint8_t risk_level, channel_status_t *out);

/**
 * @brief 부팅 직후 모든 채널을 '아직 확인되지 않음'으로 둔다. 스케줄러 시작 전에 부를 것.
 * @details 예전에는 여기서 NONE(초록)을 채웠다. 그러면 첫 HEARTBEAT부터 상행이 초록을 싣고
 * 올라가는데, 그것은 Slave에게 확인한 값이 아니라 Master의 추측이다. 두 가지가 무너진다:
 *
 *   - 정직성: Slave 전원이 꺼져 있거나 스트립이 죽어 있어도 RPi는 "초록 점등 중"으로 받는다.
 *   - 관측성: RPi(SerialHwEventDispatcher::reportIndicators)도 Qt도 '값이 바뀔 때만' 갱신한다.
 *     부팅부터 초록이면 전이가 한 번도 없어 화면에는 아무 것도 반영되지 않는다.
 *
 * 전 필드 0(= 어느 LED도 켜지 않음)으로 두면, Slave의 첫 ACK가 확인해 준 순간
 * '미확인 -> 초록' 전이가 생긴다. 그 전이 하나가 RPi를 거쳐 Qt까지 그대로 올라간다.
 * 확인이 될 때까지 초록이라고 말하지 않는 것이 이 함수의 요점이다.
 *
 * 상행 규약에 '미확인' 인코딩이 따로 없어서(led_red/yellow/green 셋뿐) 셋 다 0인 조합을
 * 그 자리에 쓴다. RPi의 decodeRiskLevel()은 이것을 None으로 읽지만 ledGreen=false 라
 * '초록 점등 확인됨'과 구분된다.
 *
 * applied_risk 를 SCHED_RISK_UNKNOWN 으로 두므로 스케줄러가 첫 틱에 불일치로 잡아
 * 평소 경로(송신 -> ACK 확인 -> 상행 ACK)로 내보낸다.
 *
 * !! 부팅값을 초록으로 되돌리지 말 것. tools/test_sched_loop.c 의
 *    test_boot_reports_only_confirmed_state() 가 정직성과 관측성을 둘 다 잡는다.
 */
void channel_boot_state_init(void);

#endif /* VEDA_CHANNEL_H */
