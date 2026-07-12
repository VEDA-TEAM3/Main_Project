/**
 * @file    FrameAggregator.h
 * @brief   4개 채널에서 비동기로 도착하는 TrackedPoint를 시간 동기화된
 *          하나의 스냅샷(Frame)으로 모으는 컴포넌트
 * @note    인터페이스가 아닌 concrete 클래스이다. 4개 채널을 취합하는 로직은
 *          채널 개수가 바뀌는 일이 드물어 알고리즘 교체 대비가 불필요하다고 판단했다.
 * @note    네트워크 수신 스레드(push 호출)와 frameIntervalMs 주기 타이머 스레드
 *          (collectFrame 호출)가 동시에 접근하므로, 내부적으로 mutex를 통해
 *          스레드 세이프를 보장한다. 호출자는 별도 동기화를 하지 않아도 된다.
 * @note    멀티 카메라(CCTV 2대 이상) 확장 시, 서로 다른 채널의 TrackedPoint가
 *          물리적으로 동일 객체를 가리키는 경우를 병합하는 로직이 필요할 수 있다.
 *          현재 스코프(CCTV 1대, 채널 간 시야 비중첩)에서는 해당 없음.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "model/TrackedPoint.h"

/**
 * @brief   채널별 TrackedPoint를 취합하여 프레임 스냅샷을 생성하는 클래스
 * @note    collectFrame()은 호출 시점까지 쌓인 값을 반환함과 동시에 이번 주기 버퍼를
 *          비운다(drain). 객체가 더 이상 패킷을 보내지 않게 되면(채널 이탈 등)
 *          별도 만료 로직 없이 자동으로 다음 프레임에서 빠진다.
 * @note    객체(channelId+objectId)별로 마지막으로 받아들인 timestamp를 별도로 기억하며,
 *          이 기록은 collectFrame() 호출로 지워지지 않고 계속 유지된다. push()로 들어온
 *          TrackedPoint의 timestamp가 이 값 이하이면 이미 처리했거나 지연/중복 도착한
 *          패킷으로 간주하여 폐기한다 (MQTT의 순서 역전, 중복 전달 대비).
 */
class FrameAggregator {
public:
    FrameAggregator() = default;

    /**
     * @brief   TrackedPoint 하나를 이번 주기 버퍼에 반영한다.
     * @note    같은 channelId+objectId 조합에 대해 이미 처리한 timestamp 이하로 들어오면
     *          폐기한다. 통과한 경우, 이번 주기 버퍼와 "마지막 처리 timestamp" 기록을
     *          모두 갱신한다.
     * @param   point   반영할 TrackedPoint
     */
    void push(const TrackedPoint& point);

    /**
     * @brief   이번 주기 버퍼에 쌓인 TrackedPoint 전체를 반환하고, 버퍼를 비운다.
     * @note    객체별 "마지막 처리 timestamp" 기록은 비우지 않고 유지한다.
     * @return  이번 주기 동안 수신된 전체 채널 TrackedPoint 목록
     */
    std::vector<TrackedPoint> collectFrame();

private:
    std::mutex mutex_; /**< push()/collectFrame() 동시 접근을 막기 위한 뮤텍스 */

    std::unordered_map<std::string, TrackedPoint> latestByKey_;
    /**< 이번 주기 버퍼. key: channelId + "_" + objectId. collectFrame() 호출 시 비워짐 */

    std::unordered_map<std::string, uint64_t> lastSeenTimestampByKey_;
    /**< 객체별 마지막 처리 timestamp. key: channelId + "_" + objectId.
         collectFrame() 호출로 지워지지 않고 영구 유지된다 */
};