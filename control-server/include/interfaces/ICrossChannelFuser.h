#pragma once

/**
 * @file ICrossChannelFuser.h
 * @brief 다중 채널 프레임을 하나의 월드 프레임으로 융합하는 인터페이스
 * @details
 * Aggregator가 모아준 여러 대의 채널 프레임 데이터를 분석하여,
 * 겹치는 시야(FOV)에 존재하는 동일 객체를 하나로 병합하고
 * 관제 서버 내부 도메인 모델(WorldFrame)로 변환.
 */

#include <vector>

#include "Contract.h"
#include "domain/WorldFrame.h"

class ICrossChannelFuser {
public:
    virtual ~ICrossChannelFuser() = default;

    /**
     * @brief 다중 채널 데이터를 단일 월드 프레임으로 융합
     * @param frames 동일한 시간 윈도우 내에 수집된 1~4채널의 프레임 목록
     * @return domain::WorldFrame 중복이 제거되고 전역 ID(GlobalId)가 부여된 교차로 전체 프레임
     */
    virtual domain::WorldFrame fuse(const std::vector<veda::TopViewFrame>& frames) = 0;
};