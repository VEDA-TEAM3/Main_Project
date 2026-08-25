#pragma once

/**
 * @file    NullTransform.h
 * @brief   가짜 구현체 (좌표를 그대로 통과)
 */

#include "interfaces/ILocalToWorldTransform.h"

class NullTransform : public ILocalToWorldTransform {
public:
    /// @warning 좌표를 그대로 복사만 함 -- 로컬 좌표가 월드 좌표인 척 통과하므로 테스트 전용
    void transform(const std::vector<veda::TopViewFrame>& in, std::vector<domain::ObservationFrame>& out) override;
};