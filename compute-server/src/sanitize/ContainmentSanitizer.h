#pragma once

/**
 * @file    ContainmentSanitizer.h
 * @brief   IoU 와 포함 관계로 중복 객체를 제거
 */

#include "interfaces/IObjectSanitizer.h"

/**
 * @brief   RISK 후보(Human, Vehicle) 중 팬텀 객체를 제거
 *
 * @details
 * - 규칙 A (부위 중복): RISK 후보가 부위 객체(Head/LicensePlate)와 IoU 가
 *   iouThresh_ 를 넘으면 제거. 카메라가 같은 신체 부위를 Head 로도 Human 으로도
 *   동시에 내보내는 경우를 잡음
 *   (실측 덤프에서 확인: ObjectId 3024 ↔ Head 3022, IoU ≈ 0.69)
 * - 규칙 B (포함 관계): 더 작은 RISK 후보가 더 큰 RISK 객체 안에 거의 완전히
 *   포함되면(IoMin > containThresh_) 작은 쪽을 제거.
 *   부위 객체가 함께 나오지 않는 중복에 대한 안전망
 *
 * likelihood 로는 필터링하지 않음
 * -- 실측 덤프에서 진짜 사람(0.46)이 중복(0.56)보다 likelihood 가 낮게 나온 사례가 존재
 */
class ContainmentSanitizer : public IObjectSanitizer {
public:
    /**
     * @brief   ContainmentSanitizer 생성자
     * @param   iouThresh       규칙 A 의 IoU 임계값 (기본 0.5)
     * @param   containThresh   규칙 B 의 포함 비율(IoMin) 임계값 (기본 0.9)
     */
    explicit ContainmentSanitizer(double iouThresh = 0.5, double containThresh = 0.9);

    domain::ChannelFrame sanitize(domain::ChannelFrame frame) override;

private:
    double iouThresh_;
    double containThresh_;
};