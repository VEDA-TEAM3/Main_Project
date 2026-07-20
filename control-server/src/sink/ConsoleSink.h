#pragma once

/**
 * @file    ConsoleSink.h
 * @brief   최종 도면 데이터를 관제 클라이언트로 전송하는 대신 콘솔에 그려주는 구현체
 */

#include "interfaces/ISink.h"

class ConsoleSink : public ISink {
public:
    ConsoleSink() = default;
    ~ConsoleSink() override = default;

    void send(const domain::WorldFrame& frame) override;
};