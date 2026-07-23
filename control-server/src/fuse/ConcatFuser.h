#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "domain/WorldPoint.h"
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
 *
 * @note [ GlobalId 안정성 ]
 * 예전에는 윈도우마다 무조건 새 gid 를 발급해서, 같은 실체가 프레임마다 다른 id 를 받았음
 * -- 앱이 객체를 추적할 수 없고, RiskFrame::nearest(gid 참조)도 프레임 간 의미가 없었음
 * -> 직전 프레임의 클러스터와 trackMaxDistance 안에서 같은 클래스면 gid 를 물려줌
 *
 * @note [ 클러스터 채널 중복 판정 ]
 * "한 클러스터에 같은 채널이 두 번 들어가면 안 된다"(= 한 카메라가 본 서로 다른 실체를
 * 하나로 합치면 안 됨)는 제약을 예전에는 매 비교마다 전체를 순회해 확인했음 -> O(n^3)
 * -> 클러스터 루트마다 채널 비트마스크를 들고 다니며 O(1) 로 판정 (전체 O(n^2))
 */
class ConcatFuser : public ICrossChannelFuser {
public:
    /**
     * @param metric             좌표 간 거리 계산기 (DI)
     * @param dedupMergeDistance 병합 판정 거리(m)
     * @param trackMaxDistance   프레임 간 같은 실체로 이어붙일 최대 이동 거리(m).
     *                           0 이하면 추적을 끄고 매번 새 gid 발급
     */
    ConcatFuser(std::shared_ptr<IDistanceMetric> metric, double dedupMergeDistance, double trackMaxDistance = 0.0);
    ~ConcatFuser() override = default;

    domain::WorldFrame fuse(const std::vector<veda::TopViewFrame>& frames) override;

private:
    /// @brief 직전 프레임에서 확정된 객체 (gid 승계용)
    struct TrackedObject {
        veda::GlobalId gid = 0;
        veda::ObjectClass cls = veda::ObjectClass::Unknown;
        domain::WorldPoint pos;
    };

    std::shared_ptr<IDistanceMetric> metric_;
    double dedupMergeDistance_;
    double trackMaxDistance_;
    std::atomic<veda::GlobalId> nextGlobalId_;

    /// @brief 직전 프레임 스냅샷 (fuse()는 Controller 단일 스레드에서만 호출됨)
    std::vector<TrackedObject> previous_;
};