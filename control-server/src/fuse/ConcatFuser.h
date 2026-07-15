#pragma once

/**
 * @file ConcatFuser.h
 * @brief 단순 병합 방식의 채널 데이터 융합기
 * @details
 * 여러 채널에서 들어온 TopViewObject들을 하나의 WorldFrame으로 모으고,
 * 각 객체에 시스템 전체에서 고유한 GlobalId를 순차적으로 부여
 */

#include <atomic>

#include "interfaces/ICrossChannelFuser.h"

class ConcatFuser : public ICrossChannelFuser {
public:
    ConcatFuser();
    ~ConcatFuser() override = default;

    /**
     * @brief 다중 채널 프레임을 단일 월드 프레임으로 병합
     */
    domain::WorldFrame fuse(const std::vector<veda::TopViewFrame>& frames) override;

private:
    std::atomic<veda::GlobalId> nextGlobalId_;  // 스레드 안전한 전역 ID 발급기
};