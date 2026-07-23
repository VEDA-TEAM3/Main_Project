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
 * @warning [ blur 경로 전용 — risk 경로에 절대 적용하지 말 것 ]
 * 이 매핑의 목적지는 '앱이 영상 위에 블러 사각형을 그리는 좌표계'임
 * 반면 호모그래피(ICoordinateTransform)는 '카메라 Metadata 이미지 평면'에서
 * 캘리브레이션되므로, 지면점을 뽑기 전에 이 매핑을 적용하면 월드 좌표가 통째로 틀어짐
 *
 * 예전에는 이 단계가 Router 앞에 있어서 risk 경로까지 매핑을 뒤집어썼음
 * -- 기본값(scale=1, offset=0)에서는 항등이라 증상이 안 보이지만, blur 정합을 맞추려고
 *    imageMapScaleX 를 건드리는 순간 월드 좌표가 조용히 어긋나는 구조였음
 * -- 그래서 Pipeline 에서 Router 뒤 blur 분기로 옮김
 *
 * @note [ 경계 판정을 하지 않는 이유 ]
 * touchesBorder/bottomTruncated 는 risk 경로가 쓰는 값이고 Metadata 좌표계 기준으로만
 * 의미가 있으므로 파서에서 한 번만 판정함. 이 인터페이스의 구현체는 건드리지 않음
 * (blur 경로의 소비자인 veda::BlurTarget 에는 경계 필드 자체가 없음)
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
     * @brief   blur 대상 객체들의 좌표를 앱 출력 좌표계로 in-place 매핑
     *
     * @param   objects     매핑 대상 객체 목록 (결과가 화면 밖이면 제거되므로 크기가 줄 수 있음)
     * @param   channelId   진단 로그용 채널 ID
     *
     * @note
     * - 반환값 대신 in-place 로 고친 뒤 축소하는 형태 -> 호출당 힙 할당 0
     * - 구현체가 항등 변환(scale=1, offset=0)을 사용하는 경우,
     *   이는 실제 화각 불일치를 보정하지 않은 상태임에 유의
     */
    virtual void map(std::vector<domain::DetectedObject>& objects, veda::ChannelId channelId) const = 0;
};
