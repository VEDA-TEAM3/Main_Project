/**
 * @file    IParser.h
 * @brief   MetadataPacket을 내부 도메인 모델로 변환하는 인터페이스
 * @note    ONVIF 등 Processor Server의 패킷 포맷이 변경될 경우를 대비해 추상화한다.
 *          구현체가 실제 파싱/필터링 로직을 담당한다.
 */
#pragma once

#include "model/ParsedResult.h"
#include "model/incoming/MetadataPacket.h"

/**
 * @brief   MetadataPacket 파싱 인터페이스
 */
class IParser {
public:
    virtual ~IParser() = default;

    /**
     * @brief   MetadataPacket 하나를 파싱하여 ParsedResult로 변환한다.
     * @note    objectType이 Human/Vehicle이면 trackedPoint를,
     *          Face/LicensePlate면 blurTarget을 채워서 반환한다.
     *          그 외 인식할 수 없는 objectType은 구현체가 정의하는 방식으로 처리한다(둘 다 비움).
     * @param   raw   Processor Server로부터 수신한 원본 메타데이터 패킷
     * @return  파싱 결과 (trackedPoint 또는 blurTarget 중 하나만 값이 채워짐)
     */
    virtual ParsedResult parse(const MetadataPacket& raw) = 0;
};