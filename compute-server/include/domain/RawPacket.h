#pragma once

/**
 * @file RawPacket.h
 * @brief Metadata raw byte
 * - IMetadataSource 의 출력, IMetadataParser 의 입력
 */

#include <chrono>
#include <cstdint>
#include <vector>

#include "Contract.h"

namespace domain {

/**
 * @struct RawPacket
 * @brief 카메라로부터 수신한 원본 메타데이터 패킷 구조체
 *
 * @note
 * - ONVIF 형식 거의 고정
 */
struct RawPacket {
    veda::ChannelId channelId = 0;  ///< 카메라 채널 ID

    /// @brief ONVIF MetadataStream XML 원본 바이트 데이터
    std::vector<std::uint8_t> bytes;

    /**
     * @brief Pi 측(수신부) 도착 시각
     * @details Δ = recvTime - (파싱된 Frame UtcTime) 측정용.
     *
     * @note
     * - LOG 용도
     */
    std::chrono::system_clock::time_point recvTime;
};

}  // namespace domain