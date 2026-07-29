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

    // 객체가 없는 프레임도 해당 시각에 위험 객체가 없다는 유효한 상태다.
    return true;
}

bool MqttTopViewSink::prepare(const veda::TopViewFrame& in, veda::TopViewFrame& out) {
    if (!isValidFrame(in)) {
        return false;
    }

    out.v = in.v;
    out.ts = in.ts;
    out.ch = in.ch;
    // out 은 직전 send() 에서 큐로 move 된 상태라 capacity 가 0 이다 -- assign 은 매번 새로 할당한다.
    // 의도된 트레이드오프이며 근거는 MqttFrameSink::prepare 의 @warning 참고
    out.objects.assign(in.objects.begin(), in.objects.end());
    return true;
}

std::string MqttTopViewSink::describe(const veda::TopViewFrame& frame) const {
    return "ch=" + std::to_string(frame.ch) + " objects=" + std::to_string(frame.objects.size());
}
