#pragma once

/**
 * @file    IMetadataSource.h
 * @brief   Metadata를 가져오는 Source 인터페이스 (블로킹 Pull 방식)
 */

#include "domain/RawPacket.h"

/**
 * @brief Metadata 입력을 추상화한 인터페이스
 */
class IMetadataSource {
public:
    virtual ~IMetadataSource() = default;

    /**
     * @brief 다음 Metadata 패킷을 가져옴
     *
     * @details
     * 패킷이 도착할 때까지 블로킹해도 됨
     *
     * @param
     * - out 원본 Metadata를 담을 RawPacket
     *
     * @return 정상 수신 시 true, 스트림 종료 시 false
     *
     * @warning
     * - false를 반환받은 뒤 다시 next()를 부르는 것은 허용되지 않음
     */
    virtual bool next(domain::RawPacket& out) = 0;
};