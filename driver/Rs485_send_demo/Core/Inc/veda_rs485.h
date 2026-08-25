/**
 * @file veda_rs485.h
 * @brief RS-485 회선(USART1). 명령 송신 · DE 방향 제어 · ACK 줄 조립.
 *
 * ```
 * sched_task --> send_channel_command() --[S1:CH1:R2\n]--> Slave
 * Slave --[A1:CH1:R2\n]--> USART1 ISR --> ack_feed_byte() --> ack_queue --> sched_task
 * ```
 *
 * ACK 줄의 **조립·해석 규칙은 이 파일이 아니라 `ack_parse.inc` 에 있다.** 추상화가 아니라
 * 검증을 위한 분리다 -- 그 코드는 ISR 문맥에서 돌아 실기에서는 관찰이 거의 불가능한데
 * (로그를 찍으면 그 자체로 오버런이 난다), 하는 일은 순수한 문자열 판별이라 호스트에서
 * 전부 검사할 수 있다. tools/test_ack_parse.c 가 같은 파일을 포함한다.
 *
 * ### 회선 위 규약
 * ```
 * 명령 : "S<슬레이브>:CH<채널>:R<위험도>\n"   (9자 + 개행)
 * ACK  : "A<슬레이브>:CH<채널>:R<위험도>\n"   (9자 + 개행)
 * ```
 * 첫 글자로 명령('S')과 ACK('A')가 갈린다. 그래서 트랜시버가 자기 송신을 되울리는
 * 구성이어도 자기 명령을 ACK로 오해하지 않는다.
 *
 * **두 숫자는 모두 1-based 다.** Slave의 process_rs485_line()이 '0' 자리를 형식 오류로
 * 버리므로 0-based 숫자를 그대로 실을 수 없다.
 */

#ifndef VEDA_RS485_H
#define VEDA_RS485_H

#include <stdint.h>

#include "main.h"
#include "veda_config.h"
#include "veda_types.h"

/** RS-485 버스. CubeMX(.ioc)가 만드는 포트라 main.c 가 소유한다. */
extern UART_HandleTypeDef huart1;

/** USART1(RS-485) 인터럽트 수신용 1바이트 버퍼. ISR 이 재무장에 쓴다. */
extern uint8_t rs485_rx_byte;

/**
 * @brief ACK 큐(ack_queue)를 만든다. osKernelInitialize() 뒤에 부를 것.
 * @retval 1 성공, 0 실패 (힙 부족)
 */
uint8_t veda_rs485_queue_create(void);

/**
 * @brief RS-485 트랜시버의 송신 드라이버를 제어할 수 있게 DE 핀을 출력으로 잡는다.
 * @details RS485_DE_ENABLED가 0이면 아무것도 하지 않는다(자동 방향 전환 트랜시버).
 * 기본 상태는 **수신(DE=Low)** 이다 -- 명령을 보내는 순간에만 버스를 잡아야 Slave의 ACK가
 * 회선에 실릴 수 있다. sched_task 가 루프에 들어가기 전에 부른다.
 */
void rs485_de_init(void);

/**
 * @brief Slave 의 ACK 수신을 연다(HAL_UART_Receive_IT). 이후 재무장은 각 콜백이 담당한다.
 * @details 스케줄러가 뜬 뒤에 부를 것 -- 큐가 준비되기 전에 바이트가 도착하면 갈 곳이 없다.
 */
void veda_rs485_rx_arm(void);

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
 * 회선 위의 두 숫자는 모두 1-based 다. RPi가 0-based를 쓰든 1-based를 쓰든
 * (RPI_CHANNEL_ID_BASE) RS-485 회선 위의 표현은 이 함수에서 하나로 고정된다.
 *
 *   index 0 -> "S1:CH1"   index 1 -> "S1:CH2"
 *   index 2 -> "S2:CH3"   index 3 -> "S2:CH4"
 *
 * !! 슬레이브 번호와 채널 번호는 같지 않다. 보드당 1채널이던 시절에는 두 숫자가 같아서
 *    하나로 계산했는데, 그 코드를 그대로 두면 CH3이 S3(존재하지 않는 보드)으로 나가
 *    조용히 사라진다.
 *
 * DE 를 내리는 시점: HAL_UART_Transmit()은 마지막 바이트의 TC(전송 완료)까지 기다린 뒤
 * 반환하므로 반환 직후 내려도 마지막 비트가 잘리지 않는다. 내려야 Slave가 ACK를 실을 수 있다.
 *
 * **sched_task에서만 부른다** -- huart1 송신자를 하나로 묶어 HAL의 gState 경합을 없앤다.
 * (예전에는 ctrl_task 가 소유했다. 스케줄러를 들이면서 송신 소유권이 통째로 옮겨왔다.)
 */
uint8_t send_channel_command(uint8_t channel_index, uint8_t risk_level);

/**
 * @brief 해석이 끝난 ACK 한 장을 sched_task 로 넘긴다. `ack_parse.inc` 의 seam 이다.
 * @details 큐가 가득 차면 버린다. RS485_ACK_REQUIRED가 0이면 sched_task가 큐를 비우지 않으므로
 * 정상적으로 가득 차며, 그때도 stat_ack_ok 집계는 계속 올라간다.
 */
void ack_queue_put(const rs485_ack_t *ack);

/**
 * @brief 큐에서 ACK 한 장을 꺼낸다. `ack_match.inc` 의 seam 이다.
 * @retval 1 받았다, 0 시간 안에 오지 않았다
 */
uint8_t ack_queue_take(rs485_ack_t *out, uint32_t timeout_ms);

/**
 * @brief 현재 시각(ms). `ack_match.inc` 의 seam 이다.
 */
uint32_t ack_now_ms(void);

/**
 * @brief RS-485로 받은 바이트 하나를 ACK 줄로 조립한다. **ISR 문맥 전용.**
 * @details 실제 조립 규칙은 ack_parse.inc 의 ack_feed_byte() 에 있고 이 함수는 그것을
 * 모듈 밖(veda_isr.c)에서 부를 수 있게 여는 얇은 창구다.
 *
 * 조립까지 ISR 에서 끝내는 이유: ACK는 최대 11바이트짜리 줄이라 한 바이트 시간을 넘기지
 * 않는다. 하행(USART6)처럼 태스크로 넘기지 않는 것은, ACK를 기다리는 sched_task 가 바로
 * 이 결과를 큐에서 꺼내야 하기 때문이다.
 */
void veda_rs485_ack_feed_byte(uint8_t byte);

/**
 * @brief 조립 중이던 ACK 조각을 버린다. UART 오류 콜백에서 부른다.
 * @details ACK 줄은 ISR 소유라 오류 콜백에서 직접 되돌려도 경합이 없다
 * (하행 상태머신을 건드리지 않는 것과 다른 점이다).
 */
void veda_rs485_ack_line_reset(void);

#endif /* VEDA_RS485_H */
