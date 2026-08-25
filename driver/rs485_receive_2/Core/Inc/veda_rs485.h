/**
 * @file veda_rs485.h
 * @brief RS-485 회선(USART1). 프레임 조립·해석 · DE 방향 제어 · ACK 송신.
 *
 * ```
 * Master --[S1:CH1:R2\n]--> USART1 ISR --> 조립 --> 해석 --> GPIO 즉시 적용
 *                                                      |
 *                                            ACK 예약 --+--> 태스크 --[A1:CH1:R2\n]--> Master
 * ```
 *
 * ### 회선 위 규약
 * ```
 * 명령 : "S<슬레이브>:CH<채널>:R<위험도>"   (항상 9자 + 개행)
 * ACK  : "A<슬레이브>:CH<채널>:R<위험도>"   (항상 9자 + 개행)
 * ```
 * **버스를 공유하므로 남의 프레임도 그대로 들어온다.** 프레임 앞의 슬레이브 번호가
 * MY_SLAVE_ID 와 다르면 버린다. 옆 Slave 의 ACK 도 들어오는데, 첫 글자가 'A' 라
 * 형식 오류로 버려진다 -- 로그에 `dropped: bad format` 으로 보이는 것이 그것이고 정상이다.
 *
 * ### 잘못된 위험도는 '거절'한다 (바꿔 적용하지 않는다)
 * 위험도 자리는 '0'(NONE) / '1'(WARNING) / '2'(DANGER) 세 글자만 받아들인다. '3', "10",
 * 255 같은 값은 규약에 없으므로 VERDICT_BAD_RISK 로 거절한다 -- 임의로 NONE 이나 DANGER 로
 * 바꿔 적용하지 않는다. **잘못된 프레임에 경광등을 끄거나 켜는 쪽으로 반응하면, 회선 오류가
 * 그대로 오동작이 된다.** 거절하면 앞 상태가 유지되고 Master는 ACK를 받지 못해 재전송
 * 경로를 탄다(형식 오류를 다루는 방식과 같다).
 *
 * 길이 검사가 값 범위 검사를 겸한다. "R10" 처럼 두 자리를 실으면 길이가 10이 되어
 * `length != 9U` 에 걸린다.
 *
 * ### GPIO 는 ISR 에서 바로 쓴다
 * GPIO 제어는 매우 빠르므로 ISR 문맥에서 바로 처리해 반응 지연을 없앤다. 블로킹인 것
 * (ACK 송신 · 로그 출력 · NeoPixel)만 태스크로 넘긴다.
 */

#ifndef VEDA_RS485_H
#define VEDA_RS485_H

#include <stdint.h>

#include "main.h"
#include "veda_config.h"

/** RS-485 버스. CubeMX(.ioc)가 만드는 포트라 main.c 가 소유한다. */
extern UART_HandleTypeDef huart1;

/**
 * process_rs485_line()의 판정 코드. 프레임이 어디서 버려졌는지 밖에서 알기 위한 것이다.
 * SLAVE_DEBUG_LOG가 0이어도 process_rs485_line()의 반환형으로 쓰이므로 항상 정의한다.
 */
#define VERDICT_APPLIED      0U  /**< 수용해서 GPIO까지 썼다 */
#define VERDICT_BAD_FORMAT   1U  /**< "S<n>:CH<n>:R<n>" 형식이 아니다 */
#define VERDICT_OTHER_SLAVE  2U  /**< 형식은 맞지만 다른 Slave 주소다 */
#define VERDICT_BAD_RISK     3U  /**< 위험도 자리가 0/1/2 가 아니다 */
#define VERDICT_OTHER_CHAN   4U  /**< 내 주소지만 내가 담당하지 않는 Channel이다 */

/** verdict_text[] 의 항목 수와 맞춰야 한다. */
#define VERDICT_COUNT        5U

/**
 * @brief Master가 보낸 "S<슬레이브>:CH<채널>:R<위험도>" 프레임 한 줄을 해석한다.
 * @param line 개행을 제외한 프레임 본문 (NUL로 끝나지 않음)
 * @param length line의 길이
 * @retval VERDICT_* 어디서 버렸는지(또는 수용했는지). SLAVE_DEBUG_LOG용 진단 값이다.
 * @details 검사 순서: 형식 -> 슬레이브 주소 -> 위험도 범위 -> 출력 표 조회.
 *
 * 출력 표에 없는 채널은 이 보드가 구동하지 않는다. 담당 범위 밖이거나, 범위 안이어도
 * 배선하지 않아 표에서 뺀 채널이 여기 걸린다. 판단 근거는 표 하나뿐이다.
 *
 * GPIO를 실제로 쓴 경우에만 ACK를 예약한다. **버려진 프레임에 ACK를 보내면 Master가
 * 적용되지 않은 명령을 적용된 것으로 기록하게 되어, ACK 기능의 목적이 통째로 사라진다.**
 */
uint8_t process_rs485_line(const char *line, uint8_t length);

/**
 * @brief 수신한 한 바이트를 프레임으로 조립하고, 개행을 만나면 해석한다. ISR 문맥 전용.
 * @details 버퍼를 넘긴 줄은 개행까지 통째로 버려 다음 프레임과 섞이지 않게 한다.
 */
void process_rs485_byte(uint8_t byte);

/**
 * @brief 조립 중이던 줄을 버린다. UART 오류 콜백에서 부른다.
 */
void veda_rs485_line_reset(void);

/**
 * @brief 마지막 수신 시각을 기록한다. ISR 문맥 전용.
 */
void veda_rs485_mark_rx(void);

/**
 * @brief 프레임이 끊긴 채 남아 있으면 폐기해 다음 프레임과 섞이지 않게 한다. 태스크 문맥 전용.
 * @details RS485_IDLE_RESET_MSEC 동안 조용했고 조립 중인 것이 있으면 버린다.
 *
 * !! 검사와 초기화를 **임계구역으로 묶는다.** 나누면 조건을 통과한 '직후' ISR 이 새 프레임의
 *    첫 바이트를 넣을 수 있고, 그것을 여기서 0 으로 덮어 그 프레임이 8자가 된다 --
 *    다음 프레임이 깨지는 것을 막으려는 코드가 드물게 스스로 그 증상을 만든다.
 *    마지막 수신 시각 읽기도 안에 있어야 한다. 밖에서 읽으면 그 직후 도착한 바이트가
 *    시각을 갱신해도 낡은 값으로 '조용하다'고 판단하게 되어 같은 경합이 남는다.
 *    BASEPRI 를 올리는 것뿐이라 USART1(우선순위 5)이 이 구간(수십 ns)에서만 밀린다.
 */
void veda_rs485_idle_service(void);

/**
 * @brief 인터럽트 수신을 연다. 태스크가 루프에 들어가기 전에 한 번 부른다.
 * @details 이후 재무장은 각 콜백이 담당한다.
 */
void veda_rs485_rx_arm(void);

/**
 * @brief 수신 콜백이 재무장에 쓰는 1바이트 버퍼의 주소를 준다.
 */
uint8_t *veda_rs485_rx_slot(void);

#if SLAVE_ACK_ENABLED

/**
 * @brief RS-485 트랜시버의 송신 드라이버를 켤 수 있게 DE 핀을 출력으로 잡는다.
 * @details RS485_DE_ENABLED가 0이면 아무것도 하지 않는다(자동 방향 전환 트랜시버).
 * 기본 상태는 **수신(DE=Low)** 이다 -- 버스를 놓아두어야 Master의 명령을 받을 수 있다.
 * 수신을 열기 전에 부를 것.
 */
void rs485_de_init(void);

/**
 * @brief 적용을 끝낸 명령을 ACK 송신용으로 예약한다. **ISR 문맥 전용.**
 * @details RS-485 송신은 블로킹(10바이트 = 약 870us)이라 ISR 안에서 하면 그동안 다음 바이트를
 * 놓쳐 오버런이 난다. 그래서 ISR은 값과 플래그만 세우고 실제 송신은 태스크가 맡는다.
 *
 * **슬롯은 하나뿐이다.** 앞 ACK를 아직 못 보냈으면 이번 것은 버린다(플래그가 곧 락 역할을
 * 한다). Master는 명령 하나를 보내고 ACK를 기다린 뒤 다음을 보내므로 정상 운용에서는
 * 겹치지 않는다. 부팅 직후 Master가 4채널을 연속으로 쏠 때만 일부가 버려지는데, 그 구간의
 * ACK는 Master도 확인하지 않는다.
 */
void veda_rs485_ack_offer(uint8_t channel, uint8_t risk_level);

/**
 * @brief 예약된 ACK가 있으면 Master로 되보낸다. 태스크 문맥 전용.
 * @details 적용한 등급을 그대로 돌려준다. ON/OFF로 줄여 보내면 Master가 WARNING과 DANGER의
 * 어긋남을 확인할 수 없다.
 *
 * DE 제어가 켜져 있으면 송신 전후로 드라이버를 열고 닫는다. HAL_UART_Transmit()은
 * 마지막 바이트의 TC(전송 완료) 플래그까지 기다린 뒤 반환하므로, 반환 직후 DE를 내려도
 * 마지막 비트가 잘리지 않는다.
 *
 * **루프에서 디버그 출력보다 먼저 부를 것** -- Master는 이 응답을 타임아웃(100ms) 안에
 * 받아야 적용을 확인할 수 있다.
 */
void veda_rs485_ack_service(void);

#endif /* SLAVE_ACK_ENABLED */

#endif /* VEDA_RS485_H */
