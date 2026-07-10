/**
 * @file    IParser.h
 * @brief   수신한 원본 패킷을 파싱하여 탐지 객체 목록을 추출하는 인터페이스
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief   파싱된 단일 탐지 객체 정보
 * @details 좌표 관련 필드(left, top, right, bottom, cx, cy)는 카메라 해상도에
 *          무관한 정규화된 값(-1.0 ~ 1.0, 화면 중앙이 0.0)
 *          Onvif 메타데이터가 픽셀 좌표 + Transformation(Translate/Scale)로 전달되더라도,
 *          파싱 단계에서 이미 정규화까지 완료되어 이 구조체에 담김
 */
struct DetectedObject {
    int id = -1;                                    /**< 객체 고유 ID */
    std::string type = "Unknown";                   /**< 객체 종류 */
    float likelihood = 0.f;                         /**< 객체 유사도 (0.0 ~ 1.0) */
    float left = 0, top = 0, right = 0, bottom = 0; /**< 정규화된 바운딩 박스 좌표 (-1.0 ~ 1.0) */
    float cx = 0, cy = 0;                           /**< 정규화된 바운딩 박스 중심 좌표 (-1.0 ~ 1.0) */
};

/**
 * @brief   하나의 프레임에서 파싱된 결과 전체
 */
struct ParsedFrame {
    int64_t timestamp_ms = 0;            /**< 프레임 타임스탬프 */
    std::vector<DetectedObject> objects; /**< 해당 프레임에서 탐지된 객체 목록 */
};

/**
 * @brief   원본 패킷을 파싱하여 ParsedFrame으로 변환하는 인터페이스
 * @details Onvif 메타데이터 양식이 변경될 가능성에 대비하여 인터페이스로 분리
 *          픽셀 좌표를 정규화된 좌표(-1.0~1.0)로 변환하는 책임도 구현체가 담당
 */
class IParser {
public:
    virtual ~IParser() = default;

    /**
     * @brief   원본 패킷을 파싱하여 타임스탬프와 탐지 객체 목록을 추출
     * @param   payload Network 계층으로부터 수신한 원본 패킷 데이터
     * @return  파싱된 프레임 정보 (타임스탬프 + 정규화된 좌표를 담은 탐지 객체 목록)
     *          파싱 실패 시 objects가 빈 벡터인 ParsedFrame을 반환
     */
    virtual ParsedFrame parse(std::string_view payload) = 0;
};

/**
 * @brief   IParser 구현체를 생성하는 팩토리 함수
 * @return  생성된 IParser 구현체에 대한 shared_ptr
 */
std::shared_ptr<IParser> createParser();