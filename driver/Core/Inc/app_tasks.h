/**
 * @file    app_tasks.h
 * @brief   Rpi <-> STM32 제어 파이프라인 태스크 (rx/control/tx/heartbeat/schedule)
 *
 *                UART IRQ
 *                    |
 *                    v
 *               rx_task (바이트 -> veda_downlink_frame_t 프레임 조립/검증)
 *                    |
 *              Command Queue (veda_risk_event_t)
 *                    |
 *                    v
 *             control_task
 *                    |
 *              channelRiskLevel[] 갱신 + Feedback Queue
 *                    |
 *                    v
 *     tx_task (veda_uplink_frame_t로 프레이밍해 전송)
 *
 *  heartbeat_task --(주기 트리거: schedule_task, 5초)--> Feedback Queue --> tx_task
 *
 *  led_task --(300ms 주기, channelRiskLevel[] 폴링)--> GPIO
 *      CH0=PB6(노랑), CH1=PB2(초록), CH2=PB1(노랑), CH3=PB15(빨강)
 *      DANGER=계속 켜짐 / WARNING=깜빡임 / NONE=꺼짐
 *      (명령을 받은 채널은 control_task가 즉시 한 번 반영하고, 이후 깜빡임은 led_task가 유지)
 */
#ifndef APP_TASKS_H
#define APP_TASKS_H

#include "main.h"
#include "cmsis_os.h"
#include "driver_protocol.h"

/* 큐/세마포어/태스크 생성(rx/control/tx/heartbeat/schedule). main()의 USER CODE BEGIN RTOS_THREADS에서 1회 호출 */
void App_TasksInit(void);

/* HAL_UART_RxCpltCallback(USART1)에서 호출: 수신 바이트 1개를 rx_task로 전달 */
void App_UartRxByteFromISR(uint8_t byte);

#endif /* APP_TASKS_H */
