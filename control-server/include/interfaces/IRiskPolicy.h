#pragma once

/**
 * @file    IRiskPolicy.h
 * @brief   프레임 내 모든 객체의 위험 레벨을 평가하는 인터페이스
 */

#include "domain/RiskEvaluation.h"
#include "domain/WorldFrame.h"

class IRiskPolicy {
public:
    virtual ~IRiskPolicy() = default;

    /**
     * @brief   프레임 내 모든 객체의 위험도를 평가
     * @param   frame 평가 대상이 되는 현재 교차로 전체 프레임
     * @param   out   판정 결과를 채울 호출자 소유 버퍼
     *
     * @pre     각 객체의 WorldObject::zoneId가 IZoneMapper에 의해 이미 배정되어 있어야 함
     *
     * @note    frame은 이 호출 후 각 객체의 riskLevel/nearestObj/nearestDist가 채워짐
     *
     * @warning 반환이 아니라 **out-parameter** 다. 구현체는 out.zoneLevels 를 resize 로
     *          맞추기만 하고 **새로 만들지 않아야** 한다 -- 호출자가 같은 버퍼를 매 프레임
     *          재사용하므로, 정상 운용(채널 수 불변)에서는 resize 가 no-op 이 되어
     *          프레임당 힙 할당이 0이 된다. 값 반환으로 되돌리면 그 성질이 사라진다.
     *          (IObjectRouter::route / ILocalToWorldTransform::transform 과 동일한 규약)
     */
    virtual void evaluate(domain::WorldFrame& frame, domain::RiskEvaluation& out) = 0;
};