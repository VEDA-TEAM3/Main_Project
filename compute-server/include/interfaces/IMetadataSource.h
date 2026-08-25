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

    /**
     * @brief 블로킹 중인 next()를 깨워 스트림을 끝냄 (멱등)
     *
     * @details
     * next()는 패킷이 올 때까지 무한정 기다리므로, 이게 없으면 종료 시그널을 받아도
     * main 루프를 빠져나올 방법이 없음
     * -- 호출 이후 next()는 남은 버퍼를 모두 내보낸 뒤 false를 반환
     *
     * @note [ 스레드 ]
     * next()를 부르는 스레드가 아닌 다른 스레드에서 호출됨 (시그널 처리 스레드 등)
     * -- 구현체는 next()와의 동시 호출에 안전해야 함
     *
     * @warning 예외를 던지지 않음
     */
    virtual void stop() noexcept = 0;
};