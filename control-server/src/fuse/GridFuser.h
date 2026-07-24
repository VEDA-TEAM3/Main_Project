#pragma once

/**
 * @file    GridFuser.h
 * @brief   ConcatFuser 와 결과가 동일한 드롭인 융합기. 단, 채널 간 매칭 broad-phase 를
 *          공간 해시 그리드로 바꿔 O(N^2) -> O(N*k) 로 낮춘 엔터프라이즈-스케일 구현체.
 *
 * @details
 * [ 왜 ConcatFuser 와 결과가 동일한가 (drop-in) ]
 * dedup 병합은 오직 dedupMergeDistance 이내의 (다른 채널 + 같은 클래스) 쌍에서만 일어난다.
 * ConcatFuser 는 전체 O(N^2) 쌍을 훑지만, 실제로 병합을 유발하는 것은 '가까운 쌍'뿐이다.
 * GridFuser 는 그 '가까운 쌍'만 그리드로 수집한 뒤, (i<j) 로 정렬해 ConcatFuser 와 완전히
 * 동일한 union-find + 채널중복 제약을 같은 순서로 적용한다 -> 같은 입력에 같은 클러스터/gid.
 * (cluster 조립·gid 추적·coasting 은 ConcatFuser 와 문자 그대로 동일하게 재현한다.)
 *
 * [ Principle #3 (할당 최소화) / #6 (캐시 지역성) ]
 * 그리드 버킷(인덱스 벡터)·후보·쌍·union-find 버퍼를 멤버로 들고 매 프레임 '해제 없이 clear()'
 * 재사용한다 -> warmup 이후 broad-phase 의 윈도우당 힙 할당이 0. 버킷은 고정 크기 평탄 배열이라
 * 캐시 접근이 예측 가능하다.
 *
 * @note fuse() 는 파이프라인 단일 스레드에서만 호출된다 -- 내부 버퍼/추적 상태에 락이 없다.
 * @note AppContext 에서 ConcatFuser 대신 그대로 주입하면 벤치마크/교체가 가능하다(생성자 동일).
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "domain/WorldPoint.h"
#include "interfaces/ICrossChannelFuser.h"
#include "interfaces/IDistanceMetric.h"

class GridFuser : public ICrossChannelFuser {
public:
    /**
     * @param metric             좌표 간 거리 계산기 (DI) — ConcatFuser 와 동일
     * @param dedupMergeDistance  병합 판정 거리(m). 그리드 셀 한 변의 크기로도 사용
     * @param trackMaxDistance    같은 실체로 이어붙일 때 sanity 상한(m). 0 이하면 추적 끔
     */
    GridFuser(std::shared_ptr<IDistanceMetric> metric, double dedupMergeDistance, double trackMaxDistance = 0.0);
    ~GridFuser() override = default;

    domain::WorldFrame fuse(const std::vector<domain::ObservationFrame>& frames) override;

private:
    /// @brief 후보 하나 (여러 채널의 객체를 한 배열로 모은 것)
    struct Candidate {
        veda::ChannelId ch = -1;
        veda::ObjectId id = 0;  ///< 채널 내부 ObjectId (채널 사이에서는 유일하지 않음)
        veda::ObjectClass cls = veda::ObjectClass::Unknown;
        domain::WorldPoint pos;
    };

    /// @brief gid 하나의 최근 추적 상태 (byGid_ 의 값) — ConcatFuser 와 동일
    struct TrackedEntity {
        veda::ObjectClass cls = veda::ObjectClass::Unknown;
        domain::WorldPoint pos;
        int missedWindows = 0;
    };

    using SourceId = std::pair<veda::ChannelId, veda::ObjectId>;
    struct SourceIdHash {
        std::size_t operator()(const SourceId& key) const noexcept {
            const std::size_t h1 = std::hash<veda::ChannelId>{}(key.first);
            const std::size_t h2 = std::hash<veda::ObjectId>{}(key.second);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }
    };

    static constexpr int kMaxMissedWindows = 5;

    /// @brief 공간 해시 버킷 개수 (2의 거듭제곱 -> 마스크로 인덱싱). 부하율 N/kBucketCount 가 k 를 좌우
    static constexpr std::uint32_t kBucketCount = 1u << 14;  // 16384

    /// @brief union-find find (path halving). ufParent_ 를 그 자리에서 압축
    std::size_t ufFind(std::size_t x);

    std::shared_ptr<IDistanceMetric> metric_;
    double dedupMergeDistance_;
    double cellSize_;  ///< 그리드 셀 한 변 = max(dedupMergeDistance_, eps)
    double trackMaxDistance_;
    std::atomic<veda::GlobalId> nextGlobalId_;

    /// @brief (channel, ObjectId) -> gid. 같은 채널의 같은 ObjectId 는 항상 같은 gid (ConcatFuser 와 동일)
    std::unordered_map<SourceId, veda::GlobalId, SourceIdHash> idIndex_;
    /// @brief gid -> 마지막 관측 상태. 거리 폴백/만료 판정의 단일 진실 공급원
    std::unordered_map<veda::GlobalId, TrackedEntity> byGid_;

    /// @name 프레임 간 재사용되는 broad-phase 버퍼 (매 프레임 clear, 재할당 없음)
    /// @{
    std::vector<Candidate> candidates_;
    std::vector<std::vector<std::uint32_t>> buckets_;             ///< 크기 kBucketCount, 각 버킷 = 후보 인덱스들
    std::vector<std::uint32_t> touchedBuckets_;                   ///< 이번 프레임에 쓴 버킷만 O(touched) 로 비움
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs_;  ///< 근접 병합 후보 쌍 (i<j)
    std::vector<std::size_t> ufParent_;                           ///< union-find 부모
    std::vector<std::uint64_t> ufMask_;                          ///< 클러스터 루트별 채널 비트마스크
    /// @}
};
