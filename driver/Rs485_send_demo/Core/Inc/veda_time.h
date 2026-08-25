/**
 * @file veda_time.h
 * @brief 상행 payload 에 싣는 시각. 32비트 틱의 접힘을 세어 64비트로 편다.
 */

#ifndef VEDA_TIME_H
#define VEDA_TIME_H

#include <stdint.h>

/**
 * @brief 부팅 후 경과 밀리초를 64비트로 돌려준다. 상행 payload의 timestamp_ms 값이다.
 * @details STM32에는 RTC 배터리가 없으므로 이 값은 벽시계(Unix epoch)가 아니라 부팅 기준이다.
 * RPi는 상행 timestamp를 dead 판정이나 불일치 판정에 쓰지 않고(수신 시각을 자체적으로 찍는다)
 * 로그 상관용으로만 쓰므로 이 정의로 충분하다. 벽시계가 필요해지면 RPi가 하행 프레임에 실어
 * 보내는 timestamp_ms로 오프셋을 잡는 것이 다음 단계다.
 *
 * osKernelGetTickCount()는 32비트라 약 49.7일에 감긴다. 접힘을 세어 시간이 뒤로 가지 않게 한다.
 * 여러 태스크가 부르므로 검사와 갱신을 임계구역(taskENTER_CRITICAL)으로 묶는다.
 *
 * configTICK_RATE_HZ == 1000 이므로 1틱 = 1ms 라는 전제 위에 있다.
 */
int64_t veda_now_ms(void);

#endif /* VEDA_TIME_H */
