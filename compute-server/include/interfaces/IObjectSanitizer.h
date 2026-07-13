#pragma once

/**
 * @file IObjectSanitizer.h
 * @brief 오탐지 객체를 필터링하고 제거하는 검증기 인터페이스
 */

#include "domain/ChannelFrame.h"

/**
 * @brief 프레임 내 감지된 객체 목록의 유효성을 검사하는 인터페이스
 */
class IObjectSanitizer {
public:
    virtual ~IObjectSanitizer() = default;

    /**
     * @brief 프레임 데이터를 검사하여 유효하지 않은 오탐지 객체를 제거.
     *
     * @note
     * 입력을 변형하지 않음.
     *
     * @param frame 검사할 원본 채널 프레임 (값 복사 전달)
     * @return domain::ChannelFrame 필터링이 완료된 새 채널 프레임 객체
     */
    virtual domain::ChannelFrame sanitize(domain::ChannelFrame frame) = 0;
};