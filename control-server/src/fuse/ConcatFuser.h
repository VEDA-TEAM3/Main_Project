#pragma once

#include <atomic>
#include <memory>

#include "interfaces/ICrossChannelFuser.h"
#include "interfaces/IDistanceMetric.h"

/**
 * @file    ConcatFuser.h
 * @brief   다중 채널 프레임을 융합하고, 채널 간 중복 감지 객체를
 *          병합(dedup)하는 구현체
 * @details
 * - 병합 비교는 서로 다른 채널에서 온 객체끼리만 수행
 * 같은 채널 내부 중복 검출은 연산 서버 책임으로 간주
 * - 병합 조건: 클래스 일치 + 거리가 dedupMergeDistance 이내
 * - 프레임 단위 판단
 * - 병합된 객체의 좌표는 병합 대상들의 평균, sourceChannels에 원본 채널 전부 기록
 */
class ConcatFuser : public ICrossChannelFuser {
public:
    /**
     * @param metric 좌표 간 거리 계산기 (DI)
     * @param dedupMergeDistance 병합 판정 거리(m)
     */
    ConcatFuser(std::shared_ptr<IDistanceMetric> metric, double dedupMergeDistance);
    ~ConcatFuser() override = default;

    domain::WorldFrame fuse(const std::vector<veda::TopViewFrame>& frames) override;

private:
    std::shared_ptr<IDistanceMetric> metric_;
    double dedupMergeDistance_;
    std::atomic<veda::GlobalId> nextGlobalId_;
};