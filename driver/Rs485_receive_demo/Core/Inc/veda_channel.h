/**
 * @file veda_channel.h
 * @brief 채널 <-> 하드웨어 대응표와 등급 적용. **등급별 정책이 모여 있는 유일한 자리다.**
 *
 * ### 등급별 출력 정책
 * ```
 *   등급       줄 조명   경광등   부저
 *   --------   -------   ------   ----
 *   NONE       초록      OFF      OFF
 *   WARNING    노랑      ON       OFF
 *   DANGER     빨강      ON       ON
 * ```
 * 이 표는 Master 의 `risk_to_status()` 와 **반드시 같아야 한다.** 정책을 바꾸면 양쪽을
 * 함께 고칠 것 -- 한쪽만 고치면 관제 서버가 실물과 다른 부저 상태를 보게 된다.
 *
 * 경광등과 부저는 더 이상 함께 움직이지 않는다. 그래서 둘을 하나의 alarm_on 으로 묶지
 * 않고 따로 계산한다. 묶어 두면 WARNING 에서도 부저가 울어 두 등급을 소리로 구분할 수 없다.
 */

#ifndef VEDA_CHANNEL_H
#define VEDA_CHANNEL_H

#include <stdint.h>

#include "main.h"
#include "neopixel.h"
#include "veda_config.h"

/**
 * @brief 한 Channel에 딸린 하드웨어 한 벌.
 * @details 배선 규격표의 "채널 구성"을 그대로 옮긴 것이다. 확장 지점은 이 표 하나이고,
 * 나머지 코드는 표를 조회만 하므로 손댈 필요가 없다.
 *
 *   채널 A : PA0 경광등 릴레이 + PB5 부저 + PA6 NeoPixel
 *   채널 B : PA1 경광등 릴레이 + PA8 부저 + PA7 NeoPixel
 *
 * 규격이 "경광등과 부저는 같은 채널에서 동시에 ON/OFF", "NeoPixel은 채널별 상태에 따라
 * 색상 표시" 로 정해져 있어, 세 가지를 한 항목에 묶어 채널 단위로 움직이게 했다.
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
 * 표의 최대 항목 수. 상태 배열(channel_risk[] 등)의 컴파일 시점 크기로 쓴다.
 *
 * 실제 항목 수(`channel_output_count`)는 이보다 적을 수 있다 -- 담당 채널 중 일부를 아직
 * 배선하지 않은 구성을 허용하기 위해서다. 표에서 뺀 채널은 VERDICT_OTHER_CHAN 으로
 * 거절되므로 조용히 사라지지 않고 로그에 남는다. veda_channel.c 의 _Static_assert 가
 * `count <= max` 를 빌드 시점에 못박는다(담당 범위보다 많은 채널을 표에 적으면 남의 채널까지
 * 구동하게 된다).
 */
#define CHANNEL_OUTPUT_MAX CHANNELS_PER_SLAVE

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
 * 각 보드의 표를 절대 채널 번호로 그대로 적어 두었다 -- 어느 보드가 무엇을 울리는지 그
 * 블록만 보면 알 수 있고, 한쪽을 고쳐도 다른 쪽에 영향이 없다.
 *
 * wiring/set_name 문자열은 USART2 로 그대로 나가므로 ASCII 로 적을 것.
 *
 * 표가 없는 보드 번호로 구우면 이 보드는 어떤 채널에도 반응하지 않는다. 그래서 조용히
 * 죽는 대신 `#error` 로 빌드를 세운다 -- "명령은 나가는데 아무 일도 안 일어나는" 상황을
 * 막기 위해서다.
 */
extern const channel_output_t channel_output[];

/** channel_output[] 의 실제 항목 수. CHANNEL_OUTPUT_MAX 이하임이 보장된다. */
extern const uint8_t channel_output_count;

/** 표에 없는 채널을 가리키는 값 */
#define CHANNEL_OUTPUT_NONE 0xFFU

/**
 * 담당 Channel별 현재 위험도(veda_risk_level_t). 인덱스는 channel_output[]과 같다.
 *
 * ON/OFF 로 압축하지 않고 등급을 그대로 들고 있는다. 경광등과 부저는 지금 두 등급에서
 * 똑같이 동작하지만, 나중에 WARNING 과 DANGER 에 다른 정책(점멸 주기 등)을 주려면
 * 여기 값이 남아 있어야 한다. 줄 조명은 이미 등급별로 색을 달리한다.
 *
 * ISR(process_rs485_line)이 쓰고 태스크(neopixel_service)가 읽으므로 volatile 이다.
 */
extern volatile uint8_t channel_risk[CHANNEL_OUTPUT_MAX];

/**
 * @brief Channel 번호로 출력 표의 자리를 찾는다.
 * @param channel 1-based Channel 번호
 * @retval CHANNEL_OUTPUT_NONE 이 보드가 구동하지 않는 채널이다
 * @details 항목이 두어 개뿐이라 선형 탐색으로 충분하다. ISR 문맥에서 불리므로 짧게 유지한다.
 */
uint8_t channel_output_index(uint8_t channel);

/**
 * @brief 표에 적힌 모든 채널의 릴레이/부저 핀을 출력으로 잡고 꺼진 상태로 둔다.
 * @details 표가 곧 배선이므로 초기화도 표를 돌며 한다 -- 채널을 늘리거나 핀을 옮겨도
 * 이 함수는 손댈 필요가 없다.
 *
 * RELAY_A(PA0)는 .ioc 에 있어 MX_GPIO_Init() 도 잡지만, 같은 설정을 다시 넣는 것이라
 * 무해하고 이 표를 유일한 근거로 남겨 둘 수 있다. 나머지 핀은 .ioc 에 없으므로 여기서만
 * 잡힌다 -- CubeMX 로 코드를 다시 생성해도 살아남는다.
 *
 * !! 핀을 출력으로 잡기 **전에** 꺼진 레벨을 먼저 실어 둔다. 순서가 바뀌면 초기화 순간에
 *    릴레이가 짧게 붙거나 부저가 짧게 울린다(active-low 모듈에서 특히 눈에 띈다).
 *
 * 릴레이는 2N7000 게이트를 미는 것이라 Low = 경광등 OFF 다. 게이트에 10k 풀다운이 붙어
 * 있어 이 핀이 뜨는 순간에도 릴레이는 붙지 않는다.
 *
 * NeoPixel 은 여기서 잡지 않는다. GPIO 가 아니라 타이머 AF 로 쓰이고 초기화에 HAL_Delay()
 * 가 필요해 태스크 문맥에서 neopixel_init() 이 따로 맡는다.
 *
 * 스케줄러가 뜨기 전에 끝내야 부팅 중에 경광등이 붙거나 부저가 울리지 않는다.
 */
void channel_hardware_init(void);

/**
 * @brief Channel 한 벌(경광등 + 부저)에 위험도를 적용하고 줄 조명 갱신을 예약한다.
 * @param channel 이 Slave가 담당하는 Channel 번호 (MY_FIRST_CHANNEL ~ MY_LAST_CHANNEL)
 * @param risk_level 적용할 veda_risk_level_t (VEDA_RISK_NONE / WARNING / DANGER)
 * @details 채널마다 하드웨어 한 벌이 통째로 따로 있으므로 다른 채널은 건드리지 않는다.
 * 부저도 자기 채널만 본다 -- 옆 채널이 켜져 있어도 이 부저는 조용하다.
 *
 * risk_level 을 ON/OFF 로 바꿔 저장하지는 않는다 -- channel_risk[] 에 등급을 그대로 남겨야
 * 줄 조명이 초록/노랑/빨강 세 색을 구분할 수 있다.
 *
 * 줄 조명은 여기서 '다시 칠해야 한다'만 예약한다. 어떤 색인지는 태스크가 channel_risk[] 를
 * 다시 읽어 정한다 -- 실제 송신은 블로킹이라 ISR에서 못 한다.
 *
 * LD2만 예외로 **보드 단위**다 -- 담당 채널 중 하나라도 위험하면 켠다. 온보드 LED라
 * 채널에 대응시킬 수 없고, "이 보드에서 지금 뭔가 울리는 중"을 한눈에 보는 용도다.
 *
 * 표에 없는 채널은 아무 일도 하지 않고 돌아온다. 호출자가 이미 걸러내지만, 이 함수만 봐도
 * 안전하도록 한 번 더 막는다.
 *
 * ISR 문맥에서 불린다. GPIO 제어는 매우 빨라 반응 지연을 없애려고 일부러 그렇게 두었다.
 */
void apply_channel_state(uint8_t channel, uint8_t risk_level);

#endif /* VEDA_CHANNEL_H */
