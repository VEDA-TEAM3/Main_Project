/**
 * @file    BlurTarget.h
 * @brief   블러 처리 대상(Face, LicensePlate)을 App(Qt)으로 즉시 전달하기 위한 모델
 * @note    위험 판단(IRiskDetector) 파이프라인에는 사용하지 않는다.
 *          IParser 단계에서 Face/LicensePlate로 필터링된 MetadataPacket이
 *          FrameAggregator를 거치지 않고 이 타입으로 변환되어 즉시 전송된다.
 * @note    x, y, halfWidth, halfHeight는 원본 CCTV 영상 프레임 기준 픽셀 좌표이다.
 *          (Top-View 변환 없음)
 * @note    App이 영상 프레임과 매칭하려면 timestamp가 필수이다.
 * @note    Face/LicensePlate 여부, objectId는 Qt 소비 목적(영역 블러 처리)에 불필요하여
 *          의도적으로 제외한다. 얼굴/번호판은 항상 둘 다 블러 처리하는 것을 전제로 한다.
 *          추후 종류별 선택적 블러 등 요구사항이 생기면 재도입 검토.
 */
#pragma once

#include <cstdint>
#include <string>

/**
 * @brief 블러 처리 대상 Metadata DTO
 * @note    channelId는 MetadataPacket과 동일하게 전역 유니크 규칙
 *          ("{cctv식별자}-ch{채널번호}")을 따른다.
 */
struct BlurTarget {
    std::string channelId;        /**< CCTV Channel Id (전역 유니크) */
    uint64_t timestamp;           /**< Frame Timestamp */
    double x, y;                  /**< Object Central Point */
    double halfWidth, halfHeight; /**< Object Bounding Box */
};