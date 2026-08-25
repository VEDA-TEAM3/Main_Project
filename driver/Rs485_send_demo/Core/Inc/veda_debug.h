/**
 * @file veda_debug.h
 * @brief USART2(ST-Link VCP, 115200 8N1)로 나가는 진단 로그.
 *
 * ### 왜 별도 회선이 필요한가
 * 하행이 바이너리로 바뀌면서 USART6 는 사람이 읽을 수 없게 됐고, 프레임이 버려질 때
 * Master 는 밖에서 볼 수 있는 흔적을 전혀 남기지 않는다. "프레임이 안 온다 / 왔는데
 * 체크섬이 깨졌다 / channel_id 가 범위 밖이다 / RS-485 송신이 실패했다"를 구분하려면
 * 상태를 밖으로 내보내는 회선이 따로 있어야 한다.
 *
 * ### 부르는 곳을 감시 태스크 하나로 제한한다
 * 여기 있는 함수는 전부 **블로킹 송신**이다. ISR 이나 실시간 경로(rx/ctrl/sched/tx)에서
 * 부르면 하행 수신이 밀려 오버런이 난다. huart2 를 쓰는 곳을 감시 태스크 하나로 묶으면
 * 송신 경합도 함께 사라지므로 락이 필요 없다 -- 진단 수단 때문에 실시간 경로에 락을
 * 들이는 것은 순서가 뒤바뀐 일이다.
 *
 * ### printf 를 쓰지 않는 이유
 * 스택과 코드 크기다. 감시 태스크 스택을 512바이트로 유지하려면 가변 인자 포맷터를
 * 들일 수 없다. 그래서 10진수 출력을 손으로 만든다.
 *
 * MASTER_DEBUG_LOG 가 0이면 전부 빈 매크로가 되어 코드가 남지 않는다.
 */

#ifndef VEDA_DEBUG_H
#define VEDA_DEBUG_H

#include <stdint.h>

#include "cmsis_os.h"
#include "veda_config.h"

#if MASTER_DEBUG_LOG

/**
 * @brief USART2(ST-Link VCP)로 문자열을 내보낸다. 감시 태스크 문맥에서만 부를 것.
 */
void dbg_print(const char *text);

/**
 * @brief 부호 없는 10진수를 찍는다.
 */
void dbg_print_u32(uint32_t value);

/**
 * @brief "<label>=<value> " 한 토막을 찍는다. `[stat]` 줄이 이것의 반복이다.
 */
void dbg_print_stat(const char *label, uint32_t value);

/**
 * @brief "[stack] ..." 한 줄. 태스크마다 '한 번이라도 남았던 스택의 최솟값'을 바이트로 찍는다.
 * @details uxTaskGetStackHighWaterMark() 는 그 태스크가 살아온 동안 스택이 가장 적게
 * 남았던 순간의 여유를 워드 단위로 돌려준다. 즉 이 값이 0에 가까우면 이미 한 번은
 * 아슬아슬했다는 뜻이다 -- 스택 넘침은 평소엔 멀쩡하다가 특정 경로에서만 터지는 고장이라
 * 잘 도는 지금 여유를 재 두는 것이 요점이다.
 *
 * 판단 기준: 여유가 **100바이트** 아래로 내려간 태스크가 있으면 그 태스크의 stack_size 를
 * 키운다. 특히 dbg_print 계열을 부르는 defaultTask 와 ACK 대기가 있는 schedTask 를 볼 것.
 *
 * 계측 대상이 아니라 진단이므로 감시 태스크에서만 부른다(huart2 송신자를 하나로 유지).
 */
void dbg_print_stack_line(void);

#else

#define dbg_print(text)             ((void)0)
#define dbg_print_u32(value)        ((void)0)
#define dbg_print_stat(label, val)  ((void)0)
#define dbg_print_stack_line()      ((void)0)

#endif /* MASTER_DEBUG_LOG */

#endif /* VEDA_DEBUG_H */
