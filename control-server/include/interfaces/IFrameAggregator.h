#pragma once

/**
 * @file IFrameAggregator.h
 * @brief 비동기 채널 프레임을 시간 윈도우 단위로 묶는 인터페이스
 * @details
 * Receiver로부터 들어오는 각 채널의 파편화된 프레임들을 모아두었다가,
 * 지정된 시간 윈도우가 닫히면 하나의 묶음으로 다음 파이프라인(Fuser)에 전달.
 */

#include <functional>
#include <vector>

#include "Contract.h"

class IFrameAggregator {
public:
    virtual ~IFrameAggregator() = default;

    /**
     * @brief 시간 윈도우 내에 모인 프레임들의 묶음
     */
    using AggregatedFrames = std::vector<veda::TopViewFrame>;

    /**
     * @brief 윈도우가 닫혀 집계가 완료되었을 때 호출될 콜백 함수 타입
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