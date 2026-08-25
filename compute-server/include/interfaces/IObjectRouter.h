#pragma once

/**
 * @file    IObjectRouter.h
 * @brief   프레임 내 객체를 용도에 맞게 분류하는 라우터 인터페이스
 *
 * @details
 * - parentId 있음 OR cls가 Head/LicensePlate   → Blur
 * - 그 외 + Human|Vehicle                      → Risk
 * - 그 외                                      → Drop
 *
 * @note
 * parentId/cls 중 하나만 맞아도 Blur로 라우팅함 (각각 파싱 실패 가능성이 독립적이라
 * 하나에만 의존하면 그 파싱이 실패했을 때 Blur 대상을 통째로 잃음)
 */

#include <vector>

#include "domain/ChannelFrame.h"
#include "domain/DetectedObject.h"

/**
 * @brief 라우팅 처리된 객체들을 분류하여 담는 결과 컨테이너
 */
struct RouteResult {
    std::vector<domain::DetectedObject> blur;  ///< Blur 경로로 전달할 객체 목록
    std::vector<domain::DetectedObject> risk;  ///< Risk 경로로 전달할 객체 목록
};

/**
 * @brief 감지된 객체의 처리 경로를 결정하는 인터페이스
 */
class IObjectRouter {
public:
    virtual ~IObjectRouter() = default;

    /**
     * @brief   프레임 내 객체들을 정의된 정책에 따라 분류
     *
     * @param   frame     분류 대상 객체들을 포함하는 원본 채널 프레임
     * @param   outResult 분류 결과를 담을 출력 파라미터 (Blur, Risk)
     *
     * @note [ 출력 파라미터인 이유 — 무할당 ]
     * 예전에는 RouteResult를 값으로 반환했는데, 그러면 프레임마다 Blur/Risk 두 벡터를
     * 새로 만들어 reserve 하므로 프레임당 힙 할당 2회가 발생했다.
     * 호출자가 소유한 버퍼를 넘겨받아 clear() 후 재사용하면 capacity가 보존되므로 warmup 이후 할당이 0이 된다.
     *
     * @warning 구현체는 반드시 outResult의 두 벡터를*clear()로 비운 뒤 채워야 한다.
     *          (호출자가 같은 버퍼를 계속 재사용하므로 이전 프레임 잔여물이 남으면 안 됨)
     *          clear()는 capacity를 유지하므로 재할당을 유발하지 않는다.
     */
    virtual void route(const domain::ChannelFrame& frame, RouteResult& outResult) = 0;
};