#include "sink/MqttTopViewSink.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr const char* kIface = "MqttTopView";
}  // namespace

MqttTopViewSink::MqttTopViewSink(std::shared_ptr<IMqttTransport> transport, const AppConfig& config)
    : MqttFrameSink<veda::TopViewFrame>(std::move(transport), veda::topic::topView(config.channelId),
                                        veda::qos::kTopView,
                                        static_cast<std::size_t>(std::max(1, config.mqttTopViewMaxQueueSize)), kIface),
      channelCount_(config.channelCount) {}

MqttTopViewSink::~MqttTopViewSink() { shutdown(); }

bool MqttTopViewSink::isValidFrame(const veda::TopViewFrame& frame) const noexcept {
    if (frame.v != veda::kSchemaVersion)
        return false;

    if (frame.ts <= 0)
        return false;

    if (frame.ch < 0 || frame.ch >= channelCount_)
        return false;

    for (const auto& object : frame.objects) {
        if (!veda::isRiskClass(object.cls))
            return false;

        if (!std::isfinite(object.pos.x) || !std::isfinite(object.pos.y))
            return false;
    }

    // 객체가 없는 프레임도 해당 시각에 위험 객체가 없다는 유효한 상태다.
    return true;
}

bool MqttTopViewSink::prepare(const veda::TopViewFrame& in, veda::TopViewFrame& out) noexcept {
    if (!isValidFrame(in))
        return false;

    out.v = in.v;
    out.ts = in.ts;
    out.ch = in.ch;
    // assign 으로 덮어써서 out 의 기존 capacity 를 재사용 (직전 발행에서 move 된 상태일 수 있음)
    out.objects.assign(in.objects.begin(), in.objects.end());
    return true;
}

std::string MqttTopViewSink::describe(const veda::TopViewFrame& frame) const {
    return "ch=" + std::to_string(frame.ch) + " objects=" + std::to_string(frame.objects.size());
}
