#pragma once

/**
 * @file IMetadataParser.h
 * @brief 메타데이터 원본 패킷을 파싱하는 인터페이스
 *
 * @note [파싱 규칙 (실측 덤프로 확인됨)]
 * - <tt:Class><tt:Type Likelihood="..."> 만 읽음.
 * - <tt:Transformation> 은 프레임마다 새로 읽어 적용.
 *   (Scale/Translate 값이 바뀔 수 있음)
 *
 * @warning [예외 처리 정책]
 * 실패 시 예외를 던지지 않음. 파싱 불가능한 패킷은 빈 ChannelFrame을 반환
 * -- 파이프라인 스레드가 잘못된 패킷 하나 때문에 죽으면 안 됨
 */

#include "domain/ChannelFrame.h"
#include "domain/RawPacket.h"

/**
 * @brief 메타데이터 입력을 추상화한 파서 인터페이스
 */
class IMetadataParser {
public:
    virtual ~IMetadataParser() = default;

    /**
     * @brief 원본 메타데이터 패킷을 파싱하여 내부용 ChannelFrame 으로 변환.
     *
     * @param raw 파싱할 원본 메타데이터 패킷 (RawPacket)
     * @return domain::ChannelFrame 파싱된 프레임 객체. (파싱 실패 시 빈 프레임을 반환)
     */
    virtual domain::ChannelFrame parse(const domain::RawPacket& raw) = 0;
};