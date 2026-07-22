#pragma once

/**
 * @file    ChannelFrame.h
 * @brief   1 frame 감지 결과를 담는 내부용 컨테이너
 *
 * @details
 * - Pipeline은 이 타입을 받아 라우팅/추출/변환을 거쳐
 * TopViewFrame 과 BlurFrame 두 개로 분류
 */

#include <vector>

#include "Contract.h"
#include "domain/DetectedObject.h"

namespace domain {

/**
 * @brief 1 frame 감지 결과를 담는 내부용 컨테이너
 */
struct ChannelFrame {
    veda::TimestampMs utcTime = 0;        ///< 프레임 UtcTime, epoch ms (CCTV 기준)
    veda::ChannelId channelId = 0;        ///< CCTV 채널 ID
    std::vector<DetectedObject> objects;  ///< 프레임 내 감지된 내부용 객체 목록
};

}  // namespace domain