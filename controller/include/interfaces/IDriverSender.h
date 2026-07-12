/**
 * @file    IDriverSender.h
 * @brief   STM32로 채널별 제어 신호를 전송하는 인터페이스
 * @note    통신 프로토콜이 아직 미정이므로 프로토콜 무관하게 추상화한다.
 *          구현체가 실제 전송 방식(인코딩, publish 등)을 담당한다.
 * @note    언제 보낼지는 이 인터페이스의 책임이 아니다.
 *          Controller가 RiskResult를 보고 전송 대상을 걸러서 넘기며,
 *          구현체는 오직 전달받은 값을 그대로 전송하는 역할만 한다.
 */
#pragma once

#include "model/outgoing/DriverPacket.h"

/**
 * @brief   STM32 전송 인터페이스
 */
class IDriverSender {
public:
    virtual ~IDriverSender() = default;

    /**
     * @brief   채널 하나에 대한 제어 패킷을 STM32로 전송한다.
     * @note    호출 여부는 Controller가 판단하여 결정한다.
     *          이 메서드는 호출되면 무조건 전송한다.
     * @param   packet   STM32로 보낼 채널 제어 패킷
     */
    virtual void sendChannelStatus(const DriverPacket& packet) = 0;
};