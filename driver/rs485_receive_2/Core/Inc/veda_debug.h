/**
 * @file veda_debug.h
 * @brief USART2(ST-Link VCP, 115200 8N1)로 나가는 진단 로그와 ISR->태스크 로그 전달 자리.
 *
 * ### 부르는 곳을 태스크 문맥으로 제한한다
 * 여기 있는 출력 함수는 전부 **블로킹 송신**이다. ISR 안에서 부르면 RS-485 수신 인터럽트가
 * 밀려 오버런이 난다. 그래서 ISR 은 해석 결과를 아래 handoff 슬롯에 놓기만 하고, 실제
 * 출력은 감시 태스크(veda_supervisor_run)가 맡는다.
 *
 * ### printf 를 쓰지 않는 이유
 * 스택과 코드 크기다 -- 가변 인자 포맷터를 들이면 이 태스크의 스택을 지금 크기로 유지할 수
 * 없다. Master 의 dbg_print_u32() 와 같은 방식이다.
 *
 * ### 실기에서 보이는 것
 * ```
 * RX "S2:CH3:R2" -> APPLIED (relay+buzzer+strip)
 * RX "S1:CH1:R2" -> dropped: another slave
 * RX "A1:CH1:R0" -> dropped: bad format      <- 옆 Slave 의 ACK를 엿들은 것. 정상이다
 * [loop] 312 turns / 5s -> 16ms each
 * ```
 *
 * SLAVE_DEBUG_LOG 가 0이면 출력 함수는 빈 매크로가 되고 handoff 도 아무 일을 하지 않는다.
 */

#ifndef VEDA_DEBUG_H
#define VEDA_DEBUG_H

#include <stdint.h>

#include "veda_config.h"

#if SLAVE_DEBUG_LOG

/**
 * @brief USART2(ST-Link VCP)로 문자열을 내보낸다. 태스크 문맥에서만 부를 것.
 */
void slave_print(const char *text);

/**
 * @brief 한 자리 숫자를 찍는다. 슬레이브 번호와 채널 번호는 모두 1~9라 이걸로 충분하다.
 */
void slave_print_digit(uint8_t value);

/**
 * @brief 부호 없는 10진수를 찍는다. 루프 주기 실측처럼 한 자리를 넘는 값에 쓴다.
 */
void slave_print_u32(uint32_t value);

/**
 * @brief ISR 이 해석을 끝낸 줄과 판정을 태스크로 넘긴다. **ISR 문맥 전용.**
 * @details 태스크가 아직 앞 줄을 출력하지 못했으면 이번 줄은 로그만 건너뛴다.
 * GPIO 처리는 호출 시점에 이미 끝났으므로 동작에는 영향이 없다.
 */
void veda_debug_line_offer(const char *line, uint8_t length, uint8_t verdict);

/**
 * @brief 넘겨받은 줄이 있으면 `RX "..." -> 판정` 한 줄로 출력한다. 태스크 문맥 전용.
 * @details 프레임이 아예 안 오는 것인지, 와서 버려지는 것인지, 받아서 GPIO까지 쓴 것인지가
 * 이 한 줄로 갈린다.
 */
void veda_debug_line_service(void);

/**
 * @brief 루프 주기 실측을 시작한다. 태스크가 루프에 들어가기 직전에 한 번 부른다.
 * @details 기준 시각을 0 으로 두면 부팅 직후 첫 바퀴에서 바로 보고가 나가 표본이 1개인
 * 엉뚱한 주기가 찍힌다.
 */
void veda_debug_loop_begin(void);

/**
 * @brief 루프를 한 바퀴 돌았음을 세고, 5초마다 `[loop]` 한 줄을 찍는다. 태스크 문맥 전용.
 * @details **왜 재는가**: ACK 는 ISR 이 아니라 메인 루프가 보낸다. 그래서 **루프 한 바퀴가
 * 곧 ACK 왕복 지연의 하한**이고, Master 의 반응 지연 계산에 그대로 들어간다. 그런데 루프
 * 안에는 osDelay(10) 말고도 블로킹인 것들이 섞여 있다(수신 로그 출력, NeoPixel DMA 대기,
 * ACK 송신). 그래서 실제 주기는 10ms 가 아니라 그보다 길고, 얼마나 긴지는 재 보기 전에는
 * 아무도 모른다 -- 호스트 시뮬레이션은 16ms 로 '가정'하고 있을 뿐이다.
 *
 * **정상값은 16ms 근처.** 40ms 를 넘으면 Master 의 지연 상한(정상 80ms)을 깨기 시작하므로
 * 그때는 로그를 줄이거나 NeoPixel 갱신을 손봐야 한다.
 *
 * 평균 주기는 정수 나눗셈이라 소수점을 버린다 -- 16 인지 40 인지만 알면 된다.
 */
void veda_debug_loop_tick(void);

#else

#define slave_print(text)                          ((void)0)
#define slave_print_digit(value)                   ((void)0)
#define slave_print_u32(value)                     ((void)0)
#define veda_debug_line_offer(line, len, verdict)  ((void)0)
#define veda_debug_line_service()                  ((void)0)
#define veda_debug_loop_begin()                    ((void)0)
#define veda_debug_loop_tick()                     ((void)0)

#endif /* SLAVE_DEBUG_LOG */

#endif /* VEDA_DEBUG_H */
