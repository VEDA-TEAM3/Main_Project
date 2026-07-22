#pragma once

/**
 * @file    IChannelReceiver.h
 * @brief   연산 서버들로부터 TopViewFrame을 수신하는 인터페이스
 *
 * @details
 * 다수의 채널에서 비동기적으로 날아오는 데이터를 받아 다음 Pipeline으로 넘김
 */

#include <functional>

#include "Contract.h"

class IChannelReceiver {
public:
    virtual ~IChannelReceiver() = default;

    /**
     * @brief 프레임이 수신되었을 때 호출될 콜백 함수 타입
     */
    using FrameCallback = std::function<void(const veda::TopViewFrame&)>;

    /**
     * @brief 프레임이 정상적으로 수신되었음을 알려줄 때 호출될 콜백 함수 타입
     */
    using AliveCallback = std::function<void(veda::ChannelId, bool /*alive*/)>;

    /**
     * @brief 수신 콜백 등록
     * @param callback 데이터가 들어올 때마다 실행할 함수
     */
    virtual void setCallback(FrameCallback callback) = 0;

    /**
     * @brief 정상 수신 콜백 등록
     * @param callback 정상적으로 수신되었음을 알려줄 때마다 실행할 함수
     */
    virtual void setAliveCallback(AliveCallback callback) = 0;

    /**
     * @brief 수신 시작
     *
     * @note 연결 실패 시 예외를 던지지 않음
     * -- 구현체 내부에서 재시도 로직을 수행
     * -- 재시도 정책(간격/횟수)은 Config
     * -- 연결 상태는 alive 콜백으로 상위에 전파
     */
    virtual void start() = 0;

    /**
     * @brief 수신 중지
     */
    virtual void stop() = 0;
};