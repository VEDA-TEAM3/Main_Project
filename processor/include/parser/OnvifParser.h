/**
 * @file    OnvifParser.h
 * @brief   ONVIF 메타데이터 XML을 파싱하는 IParser 구현체 선언
 */
#pragma once

#include "interfaces/IParser.h"

/**
 * @brief   ONVIF 메타데이터 XML을 파싱하는 IParser 구현체
 *
 * @details 카메라가 RTSP 인터리브 채널로 전송하는 <tt:MetadataStream> XML에서
 *          <tt:Frame>의 UtcTime, Transformation(Translate/Scale), 그 안의 <tt:Object> 목록을 추출
 *
 *          각 객체의 좌표(BoundingBox, CenterOfGravity)는 픽셀 단위로 오지만,
 *          Transformation 계수를 적용해 정규화된 좌표(-1.0~1.0)로 변환한 뒤
 *          DetectedObject에 담음
 *
 *          Transformation은 프레임마다 값이 바뀔 수 있다고 가정하여 매 호출마다 다시 파싱하며,
 *          별도의 상태를 갖지 않음(stateless)
 */
class OnvifParser : public IParser {
public:
    /**
     * @brief   원본 XML payload를 파싱하여 프레임 정보를 추출
     * @param   payload Network 계층으로부터 수신한 원본 패킷 (ONVIF 메타데이터 XML)
     * @return  파싱된 프레임 정보 (타임스탬프 + 정규화된 좌표를 담은 탐지 객체 목록)
     *          <tt:Frame>을 찾지 못하거나 필수 값 파싱에 실패하면 objects가 빈
     *          ParsedFrame을 반환
     */
    ParsedFrame parse(std::string_view payload) override;
};