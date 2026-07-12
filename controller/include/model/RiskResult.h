/**
 * @file    RiskResult.h
 * @brief   IRiskDetector::evaluate()의 반환 타입 및 관련 도메인 모델
 */
#pragma once

#include <string>
#include <vector>

/**
 * @brief   채널(도로 구간)의 위험 단계
 * @note    판단 기준:
 *          - CLEAR: 채널에 아무 객체도 없거나, Human 없이 Vehicle만 있고 위험 거리 이내 상황도 없음
 *          - PRESENCE: Human이 감지됨 (근접 위험 아님). Vehicle만 있으면 PRESENCE 아님.
 *          - WARNING: 8방향 최단거리가 warningDistance 미만 (객체 종류 무관)
 *          - DANGER: 8방향 최단거리가 dangerDistance 미만 (객체 종류 무관)
 */
enum class DriverLevel { CLEAR, PRESENCE, WARNING, DANGER };

/**
 * @brief   채널 하나의 위험 판단 결과
 * @note    사거리 등 채널 간 물리적 경계가 없는 구조를 전제로 하므로,
 *          level 판단은 채널 경계에 갇히지 않는다. 다른 채널에 속한 객체와 위험 거리 이내인
 *          경우에도 이 채널의 level이 WARNING/DANGER로 승격된다.
 *          8방향 최단거리 계산 자체는 항상 전체 프레임(모든 채널 합산) 기준으로 이루어진다.
 */
struct ChannelStatus {
    std::string channelId;      /**< CCTV Channel Id */
    DriverLevel level;          /**< 위험 단계 */
    std::vector<int> objectIds; /**< 이 채널 소속 객체 Id 목록 (채널 로컬 스코프) */
};

/**
 * @brief   전체 프레임(모든 채널 합산)에 대한 위험 판단 결과
 * @note    App은 위험 쌍의 구체적 위치를 표시하지 않고 채널 단위 알림 아이콘만
 *          띄우므로, 어느 객체끼리 위험 쌍인지에 대한 정보는 포함하지 않는다.
 */
struct RiskResult {
    std::vector<ChannelStatus> channelStatuses;
};