#pragma once

/**
 * @file ChannelFrame.h
 * @brief '객체별이 아닌 프레임 단위로 전송한다'는 요구사항을 타입으로 강제하는 구조체.
 *
 * @details
 * 파이프라인은 이 타입을 받아 라우팅/추출/변환을 거쳐
 * TopViewFrame 과 BlurFrame 두 개로 쪼개 내보냄.
 */

#include <vector>

#include "Contract.h"
#include "domain/DetectedObject.h"

namespace domain {

/**
 * @struct ChannelFrame
 * @brief 채널의 1 frame 감지 결과를 담는 내부용 컨테이너
 */
struct ChannelFrame {
    veda::TimestampMs utcTime = 0;        ///< ONVIF Frame UtcTime, epoch ms. 카메라 시각.
    veda::ChannelId channelId = 0;        ///< 카메라 채널 ID
    std::vector<DetectedObject> objects;  ///< 프레임 내 감지된 내부용 객체 목록
};

}  // namespace domain