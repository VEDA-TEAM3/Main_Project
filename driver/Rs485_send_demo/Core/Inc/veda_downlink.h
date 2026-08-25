/**
 * @file veda_downlink.h
 * @brief 하행(RPi -> Master, USART6). 바이트를 프레임으로 재조립하는 상태머신.
 *
 * ### 왜 ISR 에서 조립하지 않는가
 * STM32F4 USART에는 RX FIFO가 없어 ISR이 한 바이트 시간(115200 8N1 기준 약 87us)보다
 * 오래 걸리면 곧바로 오버런이 난다. 체크섬 계산(24바이트 XOR)과 큐 전달까지 ISR에 넣으면
 * 그 예산을 갉아먹으므로, ISR 은 큐에 넣는 것만 하고 상태머신은 rx_task 가 돌린다.
 *
 * ```
 * USART6 ISR --(바이트)--> rpi_rx_queue --> rx_task --(veda_risk_event_t)--> cmd_queue
 * ```
 *
 * ### 왜 START 만으로 경계를 확정하지 못하는가
 * payload 안에도 START_BYTE('S')와 같은 값이 나올 수 있다. 그래서 체크섬과 END까지 맞아야
 * 통과시킨다 -- 어긋난 경우 다음 START부터 다시 시도하면서 자연히 재동기화된다.
 * RPi의 SerialHwEventDispatcher::readerLoop() 와 대칭인 상태머신이다.
 *
 * 상태머신 자료(downlink_state 등)는 **rx_task 소유**다. ISR 이 건드리면 경합이 되므로
 * 오류 콜백에서도 되돌리지 않는다(veda_isr.h 참고).
 */

#ifndef VEDA_DOWNLINK_H
#define VEDA_DOWNLINK_H

#include <stdint.h>

#include "veda_types.h"

/**
 * @brief 하행 프레임 동기화 상태머신의 상태. RPi의 readerLoop()와 대칭이다.
 */
typedef enum
{
  DL_WAIT_START = 0,
  DL_READ_PAYLOAD,
  DL_READ_CHECKSUM,
  DL_WAIT_END
} downlink_state_t;

/** 조립 중인 위치. rx_task 소유이며 재동기화 판정에 쓰인다. */
extern downlink_state_t downlink_state;

/**
 * @brief 하행 바이트 큐(rpi_rx_queue)를 만든다. osKernelInitialize() 뒤에 부를 것.
 * @retval 1 성공, 0 실패 (힙 부족)
 */
uint8_t veda_downlink_queue_create(void);

/**
 * @brief ISR 이 받은 바이트 하나를 큐에 넣는다.
 * @retval 1 넣었다, 0 큐가 가득 찼다(호출자가 stat_rx_bytes_dropped 를 올린다)
 * @details 큐가 가득 찼다는 것은 rx_task가 밀렸다는 뜻이다. 버린 바이트는 프레임 하나를
 * 깨뜨리고 다음 프레임에서 재동기화된다.
 */
uint8_t veda_downlink_queue_put_from_isr(uint8_t byte);

/**
 * @brief 큐에서 바이트 하나를 꺼낸다. rx_task 전용.
 * @param timeout_ms 이 시간 동안 아무것도 없으면 0을 돌려준다(재동기화 판정에 쓴다)
 * @retval 1 꺼냈다, 0 시간이 지났다
 */
uint8_t veda_downlink_queue_get(uint8_t *out, uint32_t timeout_ms);

/**
 * @brief 하행 바이트 하나를 상태머신에 넣고, 프레임이 완성되면 payload를 꺼낸다.
 * @param byte 수신한 바이트
 * @param out 완성된 프레임의 payload를 받을 곳 (반환값이 1일 때만 유효)
 * @retval 1 유효한 프레임을 하나 완성했다, 0 아직이다
 * @details START를 찾을 때까지 앞의 쓰레기 바이트는 건너뛰고, payload(24B) + checksum + END가
 * 모두 맞아야 유효 프레임으로 본다. 어긋나면 조용히 버리고(stat_frames_bad) START 탐색으로
 * 돌아간다.
 *
 * rx_task 문맥에서만 부를 것 -- 상태머신 자료가 이 태스크 소유라는 전제 위에 있다.
 */
uint8_t downlink_feed_byte(uint8_t byte, veda_risk_event_t *out);

/**
 * @brief 조립 중이던 조각을 버리고 START 탐색으로 되돌린다. rx_task 의 재동기화 경로.
 * @details 프레임 중간에서 회선이 조용해졌을 때(DOWNLINK_RESYNC_MSEC) 부른다. 그대로 두면
 * 남은 조각이 다음 프레임 앞에 붙어 계속 체크섬을 깨뜨린다. stat_frames_bad 를 올린다.
 */
void veda_downlink_resync(void);

#endif /* VEDA_DOWNLINK_H */
