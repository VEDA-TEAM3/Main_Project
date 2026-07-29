#pragma once

#include <QMutex>
#include <optional>

#include "network/RiskFrameBuffer.h"

class LatestRiskFrameBuffer final : public RiskFrameBuffer {
public:
    bool submit(RiskFrameData frame) override;
    std::optional<RiskFrameData> takeLatestFrame() override;
    void cancelPendingDelivery() override;
    void clear() override;
    quint64 takeCoalescedFrameCount() override;

private:
    QMutex mutex_;
    std::optional<RiskFrameData> latestFrame_;
    quint64 coalescedFrameCount_ = 0;
    bool deliveryPending_ = false;
};
