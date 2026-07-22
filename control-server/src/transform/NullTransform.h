#pragma once

/**
 * @file    NullTransform.h
 * @brief   가짜 구현체 (좌표를 그대로 통과)
 */

#include "interfaces/ILocalToWorldTransform.h"

class NullTransform : public ILocalToWorldTransform {
public:
    void transform(std::vector<veda::TopViewFrame>& frames) override;
};