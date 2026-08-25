#pragma once

/**
 * @file    WorldObject.h
 * @brief   월드 좌표 내의 객체
 */

#include <array>
#include <cstddef>
#include <cstdint>

#include "Contract.h"
#include "domain/WorldPoint.h"

namespace domain {

/**
 * @brief   한 fused 객체에 기여한 원본 채널들의 고정 용량 인라인 집합 (힙 할당 없음)
 *
 * @details
 * 물리적으로 한 지점을 관측하는 카메라 수는 FOV 겹침으로 제한되어 보통 2~5개이며,
 * 전체 카메라 수(수백 대)와 무관하다. 즉 이 집합은 "넓은 ID 공간에서의 희소 집합"이다.
 * 그래서 대부분 0인 넓은 비트마스크(예: bitset<512> = 64B) 대신, 실제 채널 ID 만 담는
 * 소형 인라인 벡터로 저장한다:
 *  - 힙 할당 0 (구조체 안에 인라인 array) -> 융합 hot path 에서 동적 할당 없음
 *  - 희소 집합에 대해 캐시 지역성이 좋음 (~kCapacity*2B, 실제 원소만큼만 순회)
 *  - 채널 ID(<= 65535)를 그대로 보존 -> 대시보드/진단에서 "어느 카메라가 봤는지" 표시 가능
 *
 * @note kCapacity 는 "한 객체가 동시에 겹쳐 관측될 최대 채널 수"(현실적 상한)이지
 *       전체 채널 수가 아니다. 넘치는 경우 초과분은 버리고 truncated 로 표시한다
 *       (provenance 는 진단용이라 소실이 치명적이지 않음).
 */
struct SourceChannelSet {
    /// @brief 한 객체가 동시에 겹쳐 관측될 최대 채널 수 (전체 채널 수 아님)
    static constexpr std::size_t kCapacity = 8;

    std::array<std::uint16_t, kCapacity> ids{};  ///< 기여 채널 ID (앞에서부터 count 개가 유효)
    std::uint8_t count = 0;                       ///< 유효 원소 수 (<= kCapacity)
    bool truncated = false;                       ///< 용량 초과로 일부 채널을 버렸는지

    /// @brief 채널을 집합에 추가 (중복은 무시, 가득 차면 truncated 만 세우고 버림)
    void add(veda::ChannelId ch) {
        if (ch < 0)
            return;
        const auto id = static_cast<std::uint16_t>(ch);
        for (std::uint8_t i = 0; i < count; ++i) {
            if (ids[i] == id)
                return;  // 이미 존재
        }
        if (count >= kCapacity) {
            truncated = true;
            return;
        }
        ids[count++] = id;
    }

    /// @brief 채널이 집합에 포함되어 있는지
    bool contains(veda::ChannelId ch) const {
        if (ch < 0)
            return false;
        const auto id = static_cast<std::uint16_t>(ch);
        for (std::uint8_t i = 0; i < count; ++i) {
            if (ids[i] == id)
                return true;
        }
        return false;
    }

    /// @brief 기여한 채널 개수
    std::size_t size() const { return count; }
    bool empty() const { return count == 0; }
};

struct WorldObject {
    veda::GlobalId gid = 0;
    veda::ObjectClass cls = veda::ObjectClass::Unknown;
    WorldPoint pos;

    /**
     * @brief 위험 레벨
     *
     * @note
     * - 주의: cls에 따라 의미가 달라짐
     * -- Vehicle:  차량은 평가의 대상
     * -- Human:    스스로 평가되지 않으며, 차량의 nearestObj로 지목된 경우 채워짐
     */
    veda::RiskLevel riskLevel = veda::RiskLevel::None;

    veda::GlobalId nearestObj = 0;
    double nearestDist = -1.0;

    veda::ChannelId zoneId = -1;

    /**
     * @brief   이 객체에 기여한 원본 채널 집합 (dedup 병합되면 2개 이상)
     * @details 고정 용량 인라인 집합이라 융합 hot path 에서 힙 할당이 없다. SourceChannelSet 참고.
     */
    SourceChannelSet sourceChannels;
};

}  // namespace domain
