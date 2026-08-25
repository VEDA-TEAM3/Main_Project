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
#include "domain/WorldObservation.h"

class ILocalToWorldTransform {
public:
    virtual ~ILocalToWorldTransform() = default;
    /**
     * @brief 채널별 로컬 좌표 프레임을 그 채널의 카메라 캘리브레이션으로 도면 공통 월드 좌표
     *        관측치로 변환
     *
     * @param in   IFrameAggregator 가 묶어준 채널별 프레임 (카메라 로컬 좌표, 읽기 전용)
     * @param out  변환 결과. 호출자가 소유·재사용하는 버퍼이며 구현체가 clear() 후 채움
     *             (윈도우마다 새로 할당하지 않기 위해 반환값이 아닌 출력 인자로 받음)
     *
     * @note [ 타입으로 경계를 강제함 ]
     * 입력은 veda::TopViewFrame(로컬 좌표), 출력은 domain::ObservationFrame(월드 좌표)로
     * 타입 자체가 다르다. 예전처럼 같은 버퍼를 in-place 로 덮어쓰지 않으므로, 로컬 좌표와
     * 월드 좌표를 섞어 쓰는 실수는 런타임이 아니라 컴파일 타임에 걸린다
     *
     * @note 캘리브레이션이 없거나 월드 범위(worldBounds)를 벗어난 객체는 구현체가 폐기할 수 있음
     *       (로컬 좌표를 월드 좌표인 척 통과시키면 zone 배정/위험 판정이 조용히 틀어지므로)
     */
    virtual void transform(const std::vector<veda::TopViewFrame>& in,
                           std::vector<domain::ObservationFrame>& out) = 0;
};