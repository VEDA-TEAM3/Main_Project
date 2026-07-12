/**
 * @file    DriverPacket.h
 * @brief   STM32(드라이버)로 전송하는 채널별 제어 패킷
 * @note    채널에 사람이 감지되면(PRESENCE 이상) 항상 전송한다. CLEAR인 채널은
 *          전송하지 않는다 (Controller가 전송 여부를 판단하며, 이 구조체 자체는
 *          "보낼 값"만 표현한다).
 */
#pragma once

#include <string>

#include "model/RiskResult.h"

/**
 * @brief   STM32에게 보낼 Packet
 */
struct DriverPacket {
    std::string channelId; /**< CCTV Channel Id (전역 유니크) */
    DriverLevel level;     /**< LED 릴레이 제어 레벨 */
};