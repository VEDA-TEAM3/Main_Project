#pragma once

/**
 * @file    RawPacket.h
 * @brief   CCTV로부터 수신한 원본 Metadata 원본 패킷
 */

#include <chrono>
#include <cstdint>
#include <vector>

#include "Contract.h"

namespace domain {

/**
 * @brief CCTV로부터 수신한 원본 Metadata 원본 패킷
 */
struct RawPacket {
    veda::ChannelId channelId = 0;  ///< CCTV 채널 ID

    /// @brief 원본 바이트 데이터
    std::vector<std::uint8_t> bytes;

    /**
     * @brief Pi 측 도착 시각
     * @details Δ = recvTime - (패킷 내 Timestamp) 측정
     *
     * @note
     * -- Debug / Log 용도
     * -- 추후에 IClock으로 추출 가능성
     */
    std::chrono::system_clock::time_point recvTime;
};

}  // namespace domain