#pragma once

/**
 * @file    SpatialZoneMapper.h
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/IZoneMapper.h"

/**
 * @brief 월드 좌표를 하드웨어 zone 에 배정하는 구현체
 *
 * @details
 * zoneId 0~3이 모두 있으면 방향 점수 {y, x, -y, -x} 중 최댓값으로 배정한다.
 * 따라서 각 zone은 +Y, +X, -Y, -X 방향을 중심으로 90도 범위를 독점하며 AABB
 * 겹침이나 선언 순서의 영향을 받지 않는다. 점수가 같으면 낮은 zoneId가 이긴다.
 * 그 외 구성은 기존 AABB 규칙을 유지한다.
 *
 * 전역 주차장 도면 위의 SpatialZone 목록에 대해 각 객체의 월드 좌표 (x,y) 를 검사한다.
 *  - first-match wins: zones 선언 순서대로 검사해 처음 포함되는 상자의 zoneId 를 배정
 *  - 경계 포함([minX,maxX] × [minY,maxY])
 *  - 어느 상자에도 안 들면 zoneId = -1 (미배정 → 하위 단계에서 알람 대상 제외)
 *
 * [ 히스테리시스 ]
 * 경계 위에 걸친 객체는 좌표 잡음만으로도 프레임마다 zoneId 가 튄다(1프레임 진동).
 * 그 진동이 하류에서 채널 A/B 알람이 번갈아 켜지는 '중복/유령 알람'으로 나타난다.
 * 그래서 gid 별 직전 zone 을 기억해 둔다. 방향 모드는 이전 방향 점수가 최고 점수보다
 * margin 이내면 유지하고, AABB 모드는 이전 상자를 margin 만큼 넓혀 유지한다.
 *
 * @note 이 mapper 는 순수 함수가 아니다(프레임 간 상태를 갖는다). assign() 은
 *       Controller::processPipeline 단일 스레드에서만 호출되어야 한다.
 */
class SpatialZoneMapper : public IZoneMapper {
public:
    /**
     * @param zones            zone 목록. zoneId 0~3 구성은 방향 점수, 그 외는 AABB로 판정
     * @param hysteresisMargin 직전 zone 유지 여유 폭 (m). 0 이면 히스테리시스 비활성
     * @throws std::invalid_argument hysteresisMargin 이 비유한이거나 음수인 경우
     */
    explicit SpatialZoneMapper(std::vector<SpatialZone> zones, double hysteresisMargin = 0.5);
    ~SpatialZoneMapper() override = default;

    void assign(domain::WorldFrame& frame) override;

private:
    /// @brief gid 하나의 직전 zone (인덱스로 보관해 zone 설정을 O(1)로 되찾는다)
    struct GidZone {
        veda::GlobalId gid = 0;
        std::uint32_t zoneIndex = 0;
    };

    /// @brief 좌표 -> zone 인덱스 (히스테리시스 미적용 원본 판정)
    std::uint32_t resolveLinear(const domain::WorldPoint& position) const;
    std::uint32_t resolveIndexed(const domain::WorldPoint& position) const;

    /// @brief prevZones_(gid 오름차순 정렬됨) 에서 gid 의 직전 zone 인덱스를 찾는다
    std::uint32_t findPrevZoneIndex(veda::GlobalId gid) const;

    void buildDecisionIndex();

    std::vector<SpatialZone> zones_;
    std::vector<double> xEdges_;
    std::vector<double> yEdges_;
    std::vector<std::uint32_t> winnerZoneIndices_;
    std::size_t xBucketCount_ = 0;
    std::array<std::uint32_t, 4> directionalZoneIndices_{};
    bool directionalMode_ = false;

    double hysteresisMargin_ = 0.5;

    /**
     * @brief 프레임 간 zone 기억 (이중 버퍼, gid 오름차순 정렬)
     *
     * @details
     * unordered_map 이 아니라 정렬된 벡터 쌍이다. map 은 새 gid 마다 노드를 힙에
     * 할당하므로 '핫패스 무할당' 규약을 지킬 수 없다. 벡터는 clear() 가 capacity 를
     * 유지하므로 warmup 이후 할당이 0이 되고, 조회는 이진 탐색으로 O(log N) 이다.
     *
     * 매 프레임 currZones_ 를 이번 프레임 객체로만 채운 뒤 prevZones_ 와 swap 하므로,
     * 사라진 gid 는 별도 정리 없이 즉시 소거된다(누수 없음).
     */
    std::vector<GidZone> prevZones_;
    std::vector<GidZone> currZones_;
};
