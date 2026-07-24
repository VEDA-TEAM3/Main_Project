#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
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
 * -> "직전 프레임 스냅샷과 거리 비교"로 gid 를 물려주는 방식을 처음 시도했었으나, 이건
 *    본질적으로 계속 추측일 뿐이었음: compute-server(ONVIF)가 채널 내부에서는 이미
 *    프레임 간 안정적인 ObjectId 로 같은 실체를 추적하고 있는데, 그 정보를 매 윈도우
 *    버리고 월드 좌표 거리만으로 재추측하고 있었고, "직전 프레임 하나"만 기억하다 보니
 *    한 윈도우라도 감지가 비면(순간적인 가려짐 등으로 흔함) 그 즉시 추적이 끊겼음
 * -> (channel, ObjectId) -> GlobalId 를 그대로 들고 있는 영속 인덱스(idIndex_)로 교체.
 *    같은 채널이 같은 ObjectId 를 다시 보내면 좌표만 갱신하고 gid 는 그대로 유지 --
 *    "직전 프레임"이 아니라 "그 실체를 마지막으로 본 상태"를 기준으로 판단하므로 중간에
 *    몇 윈도우가 비어도(kMaxMissedWindows 유예 안이면) 안 끊김
 * -- ObjectId 만으로는 안 되는 경우가 둘 있어 distance 매칭을 폴백으로 남겨둠:
 *    (1) 채널 간 융합 -- 서로 다른 카메라가 같은 실체를 보므로 ObjectId 자체가 다름
 *    (2) ONVIF 자체 추적이 끊겨 같은 실체에 새 ObjectId 가 매겨진 경우
 *    이 경우 byGid_ 에 남아있는 각 gid 의 마지막 위치와 비교해 가장 가까운 것을 물려받되,
 *    각 현재 객체가 자신과 가장 가까운 후보만 보고 즉시 확정하면 순회 순서에 따라 실제로
 *    더 가까운 쌍을 가로채는 문제가 있어, 모든 (현재, 후보) 쌍을 거리순 정렬 후 가까운
 *    쌍부터 그리디하게 확정한다 (완전한 최적 이분 매칭은 아니지만 순서 의존성은 없음)
 * -- 두 매칭 모두 sanity 로 trackMaxDistance 상한을 적용함: ObjectId 가 일치해도 무조건
 *    신뢰하면, ONVIF 가 재접속 후 트래커 카운터를 리셋해 완전히 다른(먼) 실체에 같은 id 를
 *    재사용하는 경우까지 같은 실체로 오인하게 됨
 * -> byGid_ 의 각 항목은 이번 윈도우에 갱신되지 않으면 missedWindows 를 늘리고,
 *    kMaxMissedWindows(5)를 넘겨야 제거됨 -- 전역으로 한 번에 껐다 켜는 카운터가 아니라
 *    실체별로 독립적이라, 한 실체가 잠깐 안 보여도 다른 실체의 추적엔 영향이 없음
 * -- 그런데 gid 만 안정화하고 나니 다른 문제가 드러났음: 이번 윈도우에 감지 못 한 실체는
 *    worldFrame.objects 에 아예 안 들어가므로, 클라이언트(Qt)가 받는 RiskFrame 에서 그
 *    실체가 잠깐씩 사라졌다 나타나길 반복 -> 화면이 깜빡이는 것처럼 보임
 * -> 유예 기간(missedWindows <= kMaxMissedWindows) 안에서 이번 윈도우에 못 본 실체도
 *    byGid_ 에 남아있는 마지막 좌표 그대로 worldFrame.objects 에 채워 넣음("coast").
 *    새 필드 없이 기존 RiskObject 그대로 나가므로, 유예 기간 동안은 좌표가 그 자리에
 *    고정된 채로 계속 보이다가, 정말로 kMaxMissedWindows 를 넘겨야 화면에서도 사라짐
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
     * @param trackMaxDistance   같은 실체로 이어붙일 때 sanity 로 쓰는 최대 이동 거리(m).
     *                           0 이하면 추적을 끄고 매번 새 gid 발급
     */
    ConcatFuser(std::shared_ptr<IDistanceMetric> metric, double dedupMergeDistance, double trackMaxDistance = 0.0);
    ~ConcatFuser() override = default;

    domain::WorldFrame fuse(const std::vector<domain::ObservationFrame>& frames) override;

private:
    /// @brief gid 하나의 최근 추적 상태 (byGid_ 의 값)
    struct TrackedEntity {
        veda::ObjectClass cls = veda::ObjectClass::Unknown;
        domain::WorldPoint pos;

        /// @brief 이 gid 가 이번 윈도우에 갱신되지 않고 연속으로 넘어간 횟수
        int missedWindows = 0;
    };

    /// @brief (channel, 채널 내부 ObjectId) 쌍의 해시. ObjectId==0(미제공)은 키로 쓰지 않음
    struct SourceIdHash {
        std::size_t operator()(const std::pair<veda::ChannelId, veda::ObjectId>& key) const noexcept {
            const std::size_t h1 = std::hash<veda::ChannelId>{}(key.first);
            const std::size_t h2 = std::hash<veda::ObjectId>{}(key.second);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }
    };
    using SourceId = std::pair<veda::ChannelId, veda::ObjectId>;

    /// @brief 이 gid 가 이 윈도우에서 갱신되지 않은 채 넘어갈 수 있는 최대 연속 횟수
    static constexpr int kMaxMissedWindows = 5;

    std::shared_ptr<IDistanceMetric> metric_;
    double dedupMergeDistance_;
    double trackMaxDistance_;
    std::atomic<veda::GlobalId> nextGlobalId_;

    /// @brief (channel, ObjectId) -> gid 직접 매핑. 같은 채널의 같은 ObjectId 는 항상 같은 gid
    std::unordered_map<SourceId, veda::GlobalId, SourceIdHash> idIndex_;

    /// @brief gid -> 마지막으로 관측된 상태. ObjectId 매칭 실패 시 거리 폴백의 근거이자
    ///        만료(kMaxMissedWindows) 판정의 단일 진실 공급원 (fuse()는 단일 스레드에서만 호출됨)
    std::unordered_map<veda::GlobalId, TrackedEntity> byGid_;
};
