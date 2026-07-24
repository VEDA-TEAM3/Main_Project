#pragma once

/**
 * @file    AffineLocalToWorldTransform.h
 * @brief   채널별 위치/방향으로 로컬 좌표를 월드 좌표로 회전·이동 변환 (SE(2) 강체 변환)
 *
 * @details
 * compute-server 가 이미 '미터 단위' 카메라 로컬 지면 좌표를 주므로, 여기서 필요한 것은
 * 축척(scale)이 아니라 회전 + 평행이동뿐이다 -- 스케일을 넣으면 이중 보정이 되어 거리 임계값
 * (dedupMergeDistance / warningDistance / trackMaxDistance 등)이 조용히 틀어진다.
 * 변환 행렬식은 |det| = 1 (등거리 변환)이라 변환 전후로 거리가 보존된다
 */

#include <cstdint>
#include <optional>
#include <vector>

#include "core/AppConfig.h"
#include "interfaces/ILocalToWorldTransform.h"

class AffineLocalToWorldTransform : public ILocalToWorldTransform {
public:
    /**
     * @param calibrations     채널별 카메라 캘리브레이션 (channelId 중복 시 첫 항목이 사용됨)
     * @param dropUncalibrated 캘리브레이션이 없는 채널의 객체를 버릴지 여부 (기본 true)
     *                         false 면 로컬 좌표가 도면 좌표인 척 그대로 통과함 -- 위험
     * @param bounds           월드 좌표 유효 범위. enabled 면 범위 밖 객체를 폐기
     */
    explicit AffineLocalToWorldTransform(std::vector<CameraCalibration> calibrations, bool dropUncalibrated = true,
                                         WorldBounds bounds = {});

    void transform(const std::vector<veda::TopViewFrame>& in, std::vector<domain::ObservationFrame>& out) override;

private:
    /**
     * @brief channelId 로 바로 색인하는 O(1) 조회표 (해당 채널 캘리브레이션이 없으면 nullopt)
     * @details 예전에는 프레임마다 벡터를 선형 탐색해서 윈도우당 O(채널수^2) 이었음 --
     *          수백 채널 규모에서는 무시할 수 없어 색인으로 바꿈
     */
    std::vector<std::optional<CameraCalibration>> byChannel_;

    WorldBounds bounds_;
    bool dropUncalibrated_;

    /// @name 로그 rate-limit 용 누적 카운터 (윈도우마다 반복되므로)
    /// @{
    std::uint64_t uncalibratedCount_ = 0;
    std::uint64_t outOfBoundsCount_ = 0;
    /// @}
};
