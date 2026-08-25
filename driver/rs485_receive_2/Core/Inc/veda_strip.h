/**
 * @file veda_strip.h
 * @brief NeoPixel 줄 조명. ISR 이 예약하고 태스크가 실제로 칠한다.
 *
 * ### 왜 ISR 에서 직접 칠하지 않는가
 * neopixel_show_solid() 는 전송이 끝날 때까지 기다리는 블로킹 함수라(9픽셀 약 0.9ms)
 * ISR 안에서 부르면 그동안 RS-485 바이트를 놓쳐 오버런이 난다. ACK/디버그 로그와 같은
 * 방식으로, ISR 은 플래그만 세우고 실제 송신은 감시 태스크가 맡는다.
 *
 * 색 자체는 예약에 담지 않는다. 태스크가 그 시점의 channel_risk[] 를 다시 읽으므로,
 * 밀린 사이에 명령이 여러 번 와도 항상 마지막 상태만 나간다.
 *
 * ### 같은 색은 다시 쏘지 않는다
 * 이 시스템은 상태가 바뀔 때만 명령이 오는 구조(Master가 변화를 그대로 전달)라 주기적
 * 재출력에 기대는 복구 경로가 없고, 스트립은 한 번 받은 색을 전원이 끊길 때까지 유지한다.
 * 전원 순단으로 색이 날아가는 것까지 복구하려면 여기가 아니라 주기 재전송을 따로 두어야 한다.
 *
 * ### 소등 -> 대기 -> 목표색 2단 출력을 걷어낸 이유
 * 전환마다 깜빡여서 눈이 아프고 얻는 것이 없었다. 스트립의 래치 요건은 그 대기가 아니라
 * 프레임 자체가 채운다 -- 전송 앞뒤에 각각 300us 리셋 구간이 들어 있어(neopixel.c 의
 * [실제 출력 구간] 참고) 가장 엄한 WS2812B-V5/WS2815 의 280us 요건도 넘는다.
 */

#ifndef VEDA_STRIP_H
#define VEDA_STRIP_H

#include <stdint.h>

#include "main.h"
#include "veda_config.h"

#if NEOPIXEL_ENABLED

/**
 * @brief 표의 한 자리에 해당하는 줄 조명을 지정한 위험도 색으로 칠한다. 태스크 문맥 전용.
 * @param index channel_output[] 인덱스
 * @param risk_level 표시할 veda_risk_level_t
 * @retval HAL_OK 전송 완료
 * @retval HAL_ERROR 드라이버 오류이거나 알 수 없는 위험도다
 * @retval HAL_TIMEOUT DMA 전송이 제한 시간 안에 끝나지 않았다
 * @details 잘못된 등급은 여기까지 오지 않는다(process_rs485_line()이 먼저 거절한다).
 * 그래도 default 를 두어, 혹시 들어와도 아무 색으로도 칠하지 않고 HAL_ERROR 로 돌아온다 --
 * **소등조차 하지 않는다.** 여기서 지워 버리면 회선 오류 한 번에 멀쩡히 켜져 있던 경고
 * 표시가 사라진다. 모르는 값을 임의로 초록이나 빨강으로 바꿔 표시하면 사람이 상태를 오해한다.
 *
 * 결과를 버리지 않고 돌려주는 이유: 드라이버가 실패해도 화면에는 '색이 안 바뀐다'로만
 * 보여서, 명령이 안 온 것인지 왔는데 못 그린 것인지 구분할 수 없었다.
 * 실패하면 SLAVE_DEBUG_LOG 로 `!! strip <채널> DMA timeout / show error` 를 남긴다
 * (TIMEOUT 이면 DMA 요청이 끊긴 것 -- 타이머/DMA 배정 문제, ERROR 면 인자나 DMA 오류다).
 */
HAL_StatusTypeDef strip_show(uint8_t index, uint8_t risk_level);

/**
 * @brief 이 줄이 '지금 보여 주고 있는 색'을 기록한다. 부팅 초기 점등에서 쓴다.
 * @details strip_show() 가 성공했을 때만 부를 것. 실패해도 기록해 버리면 그 색을 다시는
 * 시도하지 않아, 한 번의 DMA 오류가 영구히 틀린 색으로 굳는다.
 */
void veda_strip_mark_shown(uint8_t index, uint8_t risk_level);

/**
 * @brief 이 줄을 다시 칠해야 한다고 예약한다. **ISR 문맥 전용.**
 */
void veda_strip_request(uint8_t index);

#if !NEOPIXEL_SELFTEST_ENABLED
/**
 * @brief 예약된 줄 조명 갱신을 실제로 송신한다. 태스크 문맥에서만 부를 것.
 * @details 보낼 것이 없거나 색이 그대로면 아무것도 하지 않는다. 각 줄은 자기 채널의
 * 위험도만 본다 -- 옆 채널이 DANGER 여도 이 줄은 자기 등급 색을 유지한다.
 *
 * !! 값을 읽기 **전에** 플래그부터 내린다. 순서가 반대면 송신하는 동안 ISR이 새로 적은
 *    상태를 플래그와 함께 지워 버려 마지막 명령이 화면에 반영되지 않는다.
 *
 * 두 줄이 동시에 바뀌면 한 루프에서 두 번 송신한다(각 0.9ms). 블로킹이지만 ACK 처리보다
 * 뒤에 있어 Master 의 왕복 시간에는 영향이 없다.
 */
void neopixel_service(void);
#endif /* !NEOPIXEL_SELFTEST_ENABLED */

#endif /* NEOPIXEL_ENABLED */

#endif /* VEDA_STRIP_H */
