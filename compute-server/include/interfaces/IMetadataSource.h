#pragma once

/**
 * @file IMetadataSource.h
 * @brief 원본 메타데이터를 가져오는 소스 인터페이스 (블로킹 Pull 방식)
 */

#include "domain/RawPacket.h"

/**
 * @brief 메타데이터 입력을 추상화한 인터페이스
 */
class IMetadataSource {
public:
    virtual ~IMetadataSource() = default;

    /**
     * @brief 다음 메타데이터 패킷을 가져옴. (블로킹 pull)
     *
     * @details
     * 패킷이 도착할 때까지 블로킹해도 됨 (호출자가 자체 스레드에서 반복 호출함).
     *
     * @param out 수신된 메타데이터를 담을 RawPacket 출력 파라미터
     * @return 정상 수신 시 true, 스트림 종료 시 false
     *
     * @warning false 를 반환받은 뒤 다시 next() 를 부르는 것은 정의되지 않은 동작.
     */
    virtual bool next(domain::RawPacket& out) = 0;
};