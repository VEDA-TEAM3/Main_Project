#pragma once

/**
 * @file    IImageCoordinateMapper.h
 * @brief   정규화 Metadata 좌표를 출력(앱 표시) 프레임의 정규화 좌표로 변환
 *
 * @details
 * Metadata 좌표계와 실제 송출되는 RTSP 프레임의 좌표계는
 * 동일한 카메라에서 나오더라도 서로 다른 화각(FOV)/crop/종횡비를 가질 수 있음
 * 두 좌표계 모두 [0,1] 범위로 정규화되어 있으나, 정규화만으로는
 * 이 화각 불일치가 해소되지 않으므로 별도의 매핑이 필요함
 *
 * @warning [ Blur 경로 전용 — Risk 경로에 절대 적용하지 말 것 ]
 * 이 매핑의 목적지는 앱이 영상 위에 블러 사각형을 그리는 좌표계임
 * 반면 호모그래피는 카메라 Metadata 이미지 평면에서 캘리브레이션되므로,
 * 지면점을 뽑기 전에 이 매핑을 적용하면 월드 좌표가 통째로 틀어짐
 */

#include <vector>

#include "Contract.h"
#include "domain/DetectedObject.h"

/**
 * @brief Metadata 좌표계를 App 좌표계로 매핑하는 인터페이스
 */
class IImageCoordinateMapper {
public:
    virtual ~IImageCoordinateMapper() = default;

    /**
     * @brief   blur 대상 객체들의 좌표를 App 출력 좌표계로 in-place 매핑
     *
     * @param   objects     매핑 대상 객체 목록 (결과가 화면 밖이면 제거되므로 크기가 줄 수 있음)
     * @param   channelId   진단 로그용 채널 ID
     *
     * @note
     * - 반환값 대신 in-place로 고친 뒤 축소하는 형태 → 호출당 힙 할당 0
     * - 구현체가 항등 변환(scale=1, offset=0)을 사용하는 경우,
     *   이는 실제 화각 불일치를 보정하지 않은 상태임에 유의
     */
    virtual void map(std::vector<domain::DetectedObject>& objects, veda::ChannelId channelId) const = 0;
};
