#pragma once

/**
 * @file    ConsoleDispatcher.h
 * @brief   STM32로 통지될 위험 이벤트를 콘솔 출력으로 시뮬레이션하는 구현체
 * @details 관제 서버는 HW 상태를 직접 결정하지 않음
 */

#include <unordered_map>

#include "Contract.h"
#include "interfaces/IHwEventDispatcher.h"

class ConsoleDispatcher : public IHwEventDispatcher {
public:
    ConsoleDispatcher() = default;
    ~ConsoleDispatcher() override = default;

    void dispatch(const domain::RiskEvaluation& eval) override;
    void setStatusCallback(StatusCallback callback) override;

private:
    std::unordered_map<veda::ChannelId, veda::RiskLevel> lastSentLevel_;
    StatusCallback statusCallback_;
};