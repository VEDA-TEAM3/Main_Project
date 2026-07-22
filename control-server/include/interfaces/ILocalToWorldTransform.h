#pragma once

/**
 * @file    ILocalToWorldTransform.h
 * @brief   로컬 좌표 → 월드 좌표로 치환하는 인터페이스
 *
 * @note
 * - 연산 서버는 도면을 모르므로 CCTV 로컬 원점 기준 좌표만 추출
 * -- 로컬 좌표를 월드 좌표(도면)로 치환하는 과정이 필요함
 */

#include <vector>

#include "Contract.h"

class ILocalToWorldTransform {
public:
    virtual ~ILocalToWorldTransform() = default;
    /**
     * @brief frames내 각 TopViewFrame 의 좌표를 그 채널의 카메라 캘리브레이션으로
     *        로컬 → 공통 월드 좌표로 변환
     * @param frames IFrameAggregator가 묶어준 채널별 프레임 목록
     */
    virtual void transform(std::vector<veda::TopViewFrame>& frames) = 0;
};