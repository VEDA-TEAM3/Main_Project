#pragma once

/**
 * @file    IImageCoordinateMapper.h
 * @brief   정규화 Metadata 좌표를 출력 프레임의 정규화 좌표로 변환
 *
 * @details
 * Metadata 좌표계와 실제 송출되는 RTSP 프레임의 좌표계는
 * 동일한 카메라에서 나오더라도 서로 다른 화각(FOV)/crop/종횡비를 가질 수 있음
 * 두 좌표계 모두 [0,1] 범위로 정규화되어 있으나, 정규화만으로는
 * 이 화각 불일치가 해소되지 않으므로 별도의 매핑이 필요함
 */

#include "domain/ChannelFrame.h"

/**
 * @brief Metadata 좌표계를 App 좌표계로 매핑하는 인터페이스
 */
class IImageCoordinateMapper {
public:
    virtual ~IImageCoordinateMapper() = default;

    /**
     * @brief   입력 프레임의 각 객체 좌표를 출력 좌표계로 매핑
     *
     * @param   frame 매핑 전 채널 프레임
     * @return  매핑 후 채널 출력 프레임
     *
     * @note
     * - 구현체가 항등 변환(scale=1, offset=0)을 사용하는 경우,
     * 이는 실제 화각 불일치를 보정하지 않은 상태임에 유의
     */
    virtual domain::ChannelFrame map(domain::ChannelFrame frame) const = 0;
};