/**
 * @file    AppPacket.h
 * @brief   App으로 전송하는 프레임 단위 패킷 (TrackedFrame)
 * @note    frameIntervalMs 주기로, Human/Vehicle 전체 좌표와 채널별 위험 레벨을 함께 전송한다.
 *          Face/LicensePlate는 이 패킷에 포함되지 않는다 (BlurTarget으로 별도 즉시 전송).
 * @note    points에 objectId, objectType이 포함되므로, 위험 쌍의 구체적 상대 객체를 별도로
 *          담지 않아도 App이 채널 단위 알림 아이콘 및 개별 객체 표시를 모두 구성할 수 있다.
 */
#pragma once

#include <vector>

#include "model/RiskResult.h"
#include "model/TrackedPoint.h"

/**
 * @brief   App으로 전송하는 프레임 데이터
 */
struct AppPacket {
    std::vector<TrackedPoint> points; /**< 이번 프레임의 전체 Human/Vehicle 좌표 (objectId, objectType 포함) */
    std::vector<ChannelStatus> channelStatuses; /**< 채널별 위험 레벨 */
};