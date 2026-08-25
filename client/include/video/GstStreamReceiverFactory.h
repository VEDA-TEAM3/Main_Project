#pragma once

#include "video/StreamReceiverFactory.h"
#include "video/VideoRuntimeConfig.h"

class GstStreamReceiverFactory final : public StreamReceiverFactory {
public:
    explicit GstStreamReceiverFactory(GstRtspReceiverConfig config);
    std::shared_ptr<StreamReceiver> create(WId outputWindowHandle) const override;

private:
    GstRtspReceiverConfig config_;
};
