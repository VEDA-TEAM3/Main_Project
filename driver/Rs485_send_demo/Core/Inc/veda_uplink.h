/**
 * @file veda_uplink.h
 * @brief 상행(Master -> RPi, USART6). 채널 상태를 19바이트 프레임으로 감싸 올려 보낸다.
 *
 * ```
 * sched_task(ACK) / hb_task(HEARTBEAT) --> uplink_queue --> tx_task --> USART6
 * ```
 *
 * ### 송신자를 tx_task 하나로 묶는 이유
 * USART6는 수신이 인터럽트, 송신이 블로킹이고 HAL이 gState/RxState를 따로 관리하므로
 * 수신과 겹쳐도 된다. 하지만 **송신자가 둘이 되면** 프레임이 바이트 단위로 섞여 RPi가
 * 어느 쪽도 재조립하지 못한다. 그래서 uplink_send() 는 tx_task 에서만 부른다.
 *
 * ### 큐가 가득 차면 버린다
 * 상행은 주기 보고라서 한 장 놓쳐도 다음 장이 곧 올라온다. 여기서 블로킹하면 ctrl_task 나
 * sched_task 가 하행 처리를 멈추게 되는데, 경광등 반응 지연이 더 나쁘다.
 * 버린 수는 stat_uplink_dropped 로 센다.
 */

#ifndef VEDA_UPLINK_H
#define VEDA_UPLINK_H

#include <stdint.h>

#include "veda_types.h"

/**
 * @brief 상행 큐(uplink_queue)를 만든다. osKernelInitialize() 뒤에 부를 것.
 * @retval 1 성공, 0 실패 (힙 부족)
 */
uint8_t veda_uplink_queue_create(void);

/**
 * @brief 지금의 채널 상태로 상행 패킷을 만들어 송신 큐에 넣는다.
 * @param channel_index 0-based 내부 채널 인덱스
 * @param reason veda_uplink_reason_t (ACK 또는 HEARTBEAT)
 * @details channel_status[] 를 뮤텍스로 감싸 스냅샷한 뒤 패킷을 조립한다. reserved0 을 0으로
 * 두는 것은 규약이라 memset 으로 먼저 지운다.
 *
 * sched_task(적용 확인 직후)와 hb_task(주기 보고) 두 곳에서 부른다.
 */
void enqueue_uplink(uint8_t channel_index, uint8_t reason);

/**
 * @brief 상행 패킷을 veda_uplink_frame_t(19B)로 감싸 USART6로 내보낸다. tx_task 전용.
 * @details 성공하면 stat_uplink_sent, 실패하면 stat_uplink_tx_fail 을 올린다.
 */
void uplink_send(const veda_uplink_packet_t *packet);

/**
 * @brief 상행 큐에서 패킷 하나를 꺼낸다. tx_task 전용.
 * @retval 1 꺼냈다, 0 (osWaitForever 를 쓰므로 사실상 나오지 않는다)
 */
uint8_t veda_uplink_queue_get(veda_uplink_packet_t *out);

#endif /* VEDA_UPLINK_H */
