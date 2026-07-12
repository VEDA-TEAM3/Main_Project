/**
 * @file    TrackedPoint.h
 * @brief   IParser 출력이자 IRiskDetector 입력으로 쓰이는 내부 도메인 모델
 */
#pragma once

#include <cstdint>
#include <string>

/**
 * @brief   객체 종류
 * @note    IParser 단계에서 Human/Vehicle만 필터링되어 넘어온다.
 *          Face/LicensePlate는 BlurTarget으로 별도 처리되며 여기 도달하지 않는다.
 */
enum class ObjectType { Human, Vehicle };

/**
 * @brief   IRiskDetector에 필요한 정보만 추출한 내부 표준 모델
 * @note    블러 처리용 바운딩 박스는 위험 판단에 사용하지 않으므로 의도적으로 포함하지 않는다.
 * @note    objectId는 채널 로컬 스코프이다 (전역 유니크 아님).
 *          (TODO: 멀티 카메라 병합 문제, 현재 스코프에서는 배제)
 */
struct TrackedPoint {
    std::string channelId; /**< CCTV Channel Id */
    uint64_t timestamp;    /**< Frame Timestamp */
    int objectId;          /**< Object Id (채널 로컬 스코프) */
    ObjectType objectType; /**< Object Type */
    double x, y;           /**< Top-view 좌표계 기준 중심 좌표 */
};