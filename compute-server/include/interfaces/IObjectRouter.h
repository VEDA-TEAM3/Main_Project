#pragma once

/**
 * @file    IObjectRouter.h
 * @brief   프레임 내 객체를 용도에 맞게 분류하는 라우터 인터페이스
 *
 * @details
 * - parentId 있음 OR cls가 Head/LicensePlate   -> blur
 * - 그 외 + Human|Vehicle                      -> risk
 * - 그 외                                      -> drop
 *
 * @note
 * parentId/cls 중 하나만 맞아도 blur로 라우팅함 (각각 파싱 실패 가능성이 독립적이라
 * 하나에만 의존하면 그 파싱이 실패했을 때 blur 대상을 통째로 잃음)
 *
 * @note [ 아키텍처 의도 ]
 * - 정책이 바뀐다면 구현체만 바꿈
 */

#include <vector>

#include "domain/ChannelFrame.h"
#include "domain/DetectedObject.h"

/**
 * @brief 라우팅 처리된 객체들을 분류하여 담는 결과 컨테이너
 */
struct RouteResult {
    std::vector<domain::DetectedObject> blur;  ///< 블러 경로로 전달할 객체 목록
    std::vector<domain::DetectedObject> risk;  ///< 위험 평가 경로로 전달할 객체 목록
};

/**
 * @brief 감지된 객체의 처리 경로를 결정하는 인터페이스
 */
class IObjectRouter {
public:
    virtual ~IObjectRouter() = default;

    /**
     * @brief   프레임 내 객체들을 정의된 정책에 따라 분류.
     *
     * @param   frame 분류 대상 객체들을 포함하는 원본 채널 프레임
     * @return  RouteResult 분류가 완료된 객체 목록 (blur, risk)
     */
    virtual RouteResult route(const domain::ChannelFrame& frame) = 0;
};