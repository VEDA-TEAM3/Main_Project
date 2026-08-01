#pragma once

#include <QtGlobal>
#include <optional>

#include "model/MqttRealtimeData.h"

class RiskFrameBuffer {
public:
    virtual ~RiskFrameBuffer() = default;

    virtual bool submit(RiskFrameData frame) = 0;
    virtual std::optional<RiskFrameData> takeLatestFrame() = 0;
    virtual void cancelPendingDelivery() = 0;
    virtual void clear() = 0;
    virtual quint64 takeCoalescedFrameCount() = 0;
};
