#pragma once

/**
 * @file    WorldFrame.h
 * @brief   특정 Timestamp에 채널이 융합된 교차로 전체의 프레임 상태
 */

#include <vector>

#include "Contract.h"
#include "domain/WorldObject.h"

namespace domain {

struct WorldFrame {
    veda::TimestampMs timestamp = 0;   ///< Timestamp (UTC)
    std::vector<WorldObject> objects;  ///< 융합이 완료된 전체 객체 목록
};

}  // namespace domain