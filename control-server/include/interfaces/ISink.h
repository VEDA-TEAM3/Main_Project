#pragma once

/**
 * @file    ISink.h
 * @brief   최종 관제 데이터 전송 인터페이스
 *
 * @details
 * 객체 융합 및 위험도 판정까지 모두 완료된 최종 도면 데이터를,
 * 그리고 채널별 하드웨어/연결 상태 변화를 시각화를 담당하는 외부 시스템(Qt 클라이언트)으로 전송
 *
 * @note [ send() 와 sendChannelStatus() 를 분리한 이유 ]
 * 갱신 성격이 다름: send()는 윈도우마다(예: 100ms) 계속 나가는 고빈도 스트림이고,
 * sendChannelStatus()는 카메라/STM32 생존 상태가 실제로 바뀔 때만 가끔 호출되는 이벤트임
 * -- 같은 커넥션(MqttTransport)을 공유하지만 발행 토픽/QoS/retained 여부가 서로 다름
 */

#include "Contract.h"
#include "domain/WorldFrame.h"

class ISink {
public:
    virtual ~ISink() = default;

    /**
     * @brief 최종 프레임 데이터를 외부 대시보드로 전송
     * @param frame 모든 처리가 완료된 교차로 전체 프레임
     */
    virtual void send(const domain::WorldFrame& frame) = 0;

    /**
     * @brief   채널의 카메라/하드웨어 생존 상태 변화를 외부 대시보드로 전송
     * @param   status 채널 ID와 cameraAlive(MQTT LWT)/hardwareAlive(STM32 하트비트) 상태
     * @details Controller가 상태 전환을 감지했을 때만 호출함 (호출 빈도 낮음)
     */
    virtual void sendChannelStatus(const veda::ChannelStatus& status) = 0;
};
