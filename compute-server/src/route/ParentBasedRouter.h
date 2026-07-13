#pragma once

/**
 * @file    ParentBasedRouter.h
 * @brief   Parent 속성 유무로 BLUR/RISK 를 라우팅
 */

#include "interfaces/IObjectRouter.h"

/**
 * @brief   parentId 유무만으로 라우팅
 *
 * @details
 * 규칙:
 *   - parentId 있음                       -> blur (Head, LicensePlate)
 *   - parentId 없음 + Human|Vehicle       -> risk
 *   - 그 외                               -> drop
 */
class ParentBasedRouter : public IObjectRouter {
public:
    RouteResult route(const domain::ChannelFrame& frame) override;
};