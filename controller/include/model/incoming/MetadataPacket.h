/**
 * @file    MetadataPacket.h
 * @brief   Processor Server에서 받는 Packet 정의
 */
#pragma once

#include <cstdint>
#include <string>

/**
 * @brief   Metadata DTO
 * @note    channelId는 Processor Server에서 전역 유니크하게 생성하여 보낸다.
 *          네이밍 규칙: "{cctv식별자}-ch{채널번호}" (예: "cctv01-ch1").
 * @note    objectType 값에 따라 halfWidth/halfHeight의 유효성이 다르다.
 *          - Human/Vehicle: bbox 필드는 비어있음(미사용)
 *          - Face/LicensePlate: bbox 필드가 채워져 있으며 블러 처리에 사용됨
 */
struct MetadataPacket {
    std::string channelId;        /**< CCTV Channel Id (전역 유니크) */
    uint64_t timestamp;           /**< Frame TimeStamp */
    int objectId;                 /**< Object Id (채널 로컬 스코프) */
    std::string objectType;       /**< Object Type (Human/Vehicle/Face/LicensePlate) */
    double x, y;                  /**< Object Central Point */
    double halfWidth, halfHeight; /**< Object Bounding Box (Human/Vehicle인 경우 비어있음) */
};