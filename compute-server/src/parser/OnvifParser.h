#pragma once

/**
 * @file    OnvifParser.h
 * @brief   ONVIF 메타데이터 파서
 */

#include "domain/ChannelFrame.h"
#include "domain/RawPacket.h"
#include "interfaces/IMetadataParser.h"

/**
 * @brief 수신된 ONVIF 원본 XML 메타데이터를 파싱
 */
class OnvifParser : public IMetadataParser {
public:
    /**
     * @brief   ONVIF 메타데이터 원본 패킷을 파싱하여 내부 파이프라인용 데이터로 변환
     *
     * @param   raw ONVIF XML 원본 바이트와 메타데이터가 담긴 원시 패킷
     * @return  domain::ChannelFrame 추출된 객체 정보가 담긴 프레임
     *          오류나 예외 발생 시 파이프라인 중단을 막기 위해 빈 프레임을 반환
     */
    domain::ChannelFrame parse(const domain::RawPacket& raw) override;
};