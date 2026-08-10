#include "sink/MqttTopViewSink.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr const char* kIface = "MqttTopView";
constexpr std::size_t kMaxObjectsPerFrame = 256;
}  // namespace

MqttTopViewSink::MqttTopViewSink(std::shared_ptr<IMqttTransport> transport, const AppConfig& config)
    : MqttFrameSink<veda::TopViewFrame>(std::move(transport), veda::topic::topView(config.channelId),
                                        veda::qos::kTopView,
                                        static_cast<std::size_t>(std::max(1, config.mqttTopViewMaxQueueSize)), kIface),
      channelId_(config.channelId) {}

MqttTopViewSink::~MqttTopViewSink() { shutdown(); }

bool MqttTopViewSink::isValidFrame(const veda::TopViewFrame& frame) const noexcept {
    if (frame.v != veda::kSchemaVersion) {
        return false;
    }

    if (frame.ts <= 0) {
        return false;
    }

    if (frame.ch != channelId_) {
        return false;
    }

    if (frame.objects.size() > kMaxObjectsPerFrame) {
        return false;
    }

    for (const auto& object : frame.objects) {
        if (!veda::isRiskClass(object.cls)) {
            return false;
        }

        if (!std::isfinite(object.pos.x) || !std::isfinite(object.pos.y)) {
            return false;
        }
    }

    return true;
}

bool MqttTopViewSink::prepare(const veda::TopViewFrame& in, veda::TopViewFrame& out) {
    if (!isValidFrame(in)) {
        return false;
    }

    out.v = in.v;
    out.ts = in.ts;
    out.ch = in.ch;
    // out은 직전 send()에서 큐로 move된 상태라 capacity가 0 이다.
    // -- assign은 매번 새로 할당한다.
    out.objects.assign(in.objects.begin(), in.objects.end());
    return true;
}

std::string MqttTopViewSink::describe(const veda::TopViewFrame& frame) const {
    return "ch=" + std::to_string(frame.ch) + " objects=" + std::to_string(frame.objects.size());
}