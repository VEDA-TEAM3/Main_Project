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
     *        로컬 → 공통 월드 좌표로 변환 (in-place)
     * @param frames IFrameAggregator가 묶어준 채널별 프레임 목록
     *
     * @warning [ 반환 후 pos 의 의미가 바뀜 ]
     * TopViewObject::pos 의 타입은 veda::LocalPoint 지만, 이 함수가 반환한 뒤에는
     * 그 자리에 '도면 공통 월드 좌표'가 들어 있음 (수신 버퍼를 작업 버퍼로 재사용)
     * -- 즉 이 호출 이후의 pos 를 로컬 좌표로 해석하면 안 됨
     * -- 이 애매함이 유효한 구간은 Controller::processPipeline 안의
     *    transform() ~ fuse() 사이 한 스텝뿐이며, fuse() 를 지나면 domain::WorldPoint 로
     *    제대로 타입이 잡힘
     *
     * @note 캘리브레이션이 없는 채널의 객체는 구현체가 폐기할 수 있음
     *       (로컬 좌표를 월드 좌표인 척 통과시키면 위험 판정이 조용히 틀어지므로)
     */
    virtual void transform(std::vector<veda::TopViewFrame>& frames) = 0;
};