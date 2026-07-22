#pragma once

/**
 * @file    ParentBasedRouter.h
 * @brief   Parent 속성 또는 클래스(Head/LicensePlate)로 BLUR/RISK 를 라우팅
 */

#include "interfaces/IObjectRouter.h"

/**
 * @brief   parentId 유무와 클래스, 두 신호 중 하나만 맞아도 라우팅
 *
 * @details
 * 규칙:
 *   - parentId 있음 OR cls가 Head/LicensePlate   -> blur
 *   - 그 외 + Human|Vehicle                      -> risk
 *   - 그 외                                      -> drop
 *
 * @note
 * parentId(Parent 속성 파싱)와 cls(Type 문자열 파싱)는 서로 독립적으로 실패할 수 있음
 * (예: Parent 속성이 없거나, Type이 objectClassFromString이 모르는 문자열인 경우)
 * 하나만 의존하면 그 파싱이 실패했을 때 blur 대상을 통째로 잃으므로, 두 신호 중
 * 하나라도 맞으면 blur로 보냄 -> 둘 다 동시에 실패해야만 drop됨
 */
class ParentBasedRouter : public IObjectRouter {
public:
    RouteResult route(const domain::ChannelFrame& frame) override;
};