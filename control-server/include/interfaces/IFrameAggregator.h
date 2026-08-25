#pragma once

/**
 * @file    IFrameAggregator.h
 * @brief   비동기 채널 프레임을 시간 윈도우 단위로 묶는 인터페이스
 *
 * @details
 * Receiver로부터 들어오는 각 채널의 파편화된 프레임들을 모아두었다가,
 * 지정된 시간 윈도우가 닫히면 하나의 묶음으로 다음 Pipeline에 전달
 */

#include <functional>
#include <vector>

#include "Contract.h"

class IFrameAggregator {
public:
    virtual ~IFrameAggregator() = default;

    /** @brief 시간 기반 집계 작업을 시작한다. 동기식 구현은 기본 no-op을 사용한다. */
    virtual void start() {}

    /** @brief 시간 기반 집계 작업을 중지하고 대기 중인 작업을 정리한다. */
    virtual void stop() {}

    /**
     * @brief 시간 윈도우 내에 모인 프레임들의 묶음
     */
    using AggregatedFrames = std::vector<veda::TopViewFrame>;

    /**
     * @brief 윈도우가 닫혀 집계가 완료되었을 때 호출될 콜백 함수 타입
     *
     * @warning 값이 아니라 const 참조로 받는다. 집계기가 이 버퍼를 '풀에서 빌려' 넘기고
     *          콜백이 반환하는 즉시 회수하기 때문이다 -- 그래서 윈도우마다 새 벡터를 만들지 않는다.
     *          구현체는 이 참조를 **저장하거나 콜백 밖으로 넘기면 안 된다.** 필요하면 복사할 것.
     *          (반환 후 버퍼는 다음 윈도우에서 덮어써진다)
     */
    using AggregationCallback = std::function<void(const AggregatedFrames&)>;

    /**
     * @brief 집계 완료 콜백 등록
     * @param callback 윈도우 단위 집계가 끝날 때마다 실행할 함수
     */
    virtual void setCallback(AggregationCallback callback) = 0;

    /**
     * @brief Receiver로부터 수신된 단일 채널 프레임을 주입
     * @param frame 수신된 1채널 분량의 프레임 데이터
     */
    virtual void push(const veda::TopViewFrame& frame) = 0;
};