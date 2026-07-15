#pragma once

#include <vector>

#include "Contract.h"
#include "domain/WorldPoint.h"

namespace domain {

struct WorldObject {
    veda::GlobalId gid = 0;
    veda::ObjectClass cls = veda::ObjectClass::Unknown;
    WorldPoint pos;

    // --- IRiskPolicy 가 채우는 필드 (dedup·zone 배정 이후 실행) ---
    veda::RiskLevel riskLevel = veda::RiskLevel::None;

    /* 있어야 할 필요가 있는지 고민해봐야 함 */
    veda::GlobalId nearestObj = 0;
    double nearestDist = -1.0;
    /* end */

    // --- IZoneMapper 가 채우는 필드 (dedup 이후, RiskPolicy 이전 실행) ---
    veda::ChannelId zoneId = -1;  ///< 액추에이터 귀속 채널. -1 = 미배정(zone 밖 등 예외)

    // --- ICrossChannelFuser 가 채우는 필드 (dedup 결과 기록용) ---
    std::vector<veda::ChannelId> sourceChannels;  ///< 이 객체를 감지한 원본 채널(들).
                                                  ///< dedup 병합되면 2개 이상. 로그/디버깅 전용, 제어 로직에 사용 금지.
};

}  // namespace domain