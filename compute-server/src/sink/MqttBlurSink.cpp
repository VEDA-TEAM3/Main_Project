#include "sink/MqttBlurSink.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr const char* kIface = "MqttBlur";
constexpr std::size_t kMaxBlurTargetsPerFrame = 256;
}  // namespace

MqttBlurSink::MqttBlurSink(std::shared_ptr<IMqttTransport> transport, const AppConfig& config)
    : MqttFrameSink<veda::BlurFrame>(std::move(transport), veda::topic::blur(config.channelId), veda::qos::kBlur,
                                     static_cast<std::size_t>(std::max(1, config.mqttBlurMaxQueueSize)), kIface),
      channelId_(config.channelId) {}

MqttBlurSink::~MqttBlurSink() { shutdown(); }

bool MqttBlurSink::isValidFrame(const veda::BlurFrame& frame) const noexcept {
    if (frame.v != veda::kSchemaVersion) {
        return false;
    }

    if (frame.ts <= 0) {
        return false;
    }

    if (frame.ch != channelId_) {
        return false;
    }

    if (frame.blurs.size() > kMaxBlurTargetsPerFrame) {
        return false;
    }

    return true;
}

bool MqttBlurSink::isValidBlurTarget(const veda::BlurTarget& blur) const noexcept {
    if (!veda::isBlurClass(blur.cls)) {
        return false;
    }

    const auto& box = blur.box;
    if (!std::isfinite(box.l) || !std::isfinite(box.t) || !std::isfinite(box.r) || !std::isfinite(box.b)) {
        return false;
    }

    if (box.l < 0.0 || box.l > 1.0 || box.t < 0.0 || box.t > 1.0 || box.r < 0.0 || box.r > 1.0 || box.b < 0.0 ||
        box.b > 1.0) {
        return false;
    }

    if (box.l > box.r || box.t > box.b) {
        return false;
    }

    return true;
}

bool MqttBlurSink::prepare(const veda::BlurFrame& in, veda::BlurFrame& out) {
    if (!isValidFrame(in)) {
        return false;
    }
    out.v = in.v;
    out.ts = in.ts;
    out.ch = in.ch;

    // 개별 Blur 대상 중 클래스/좌표가 이상한 것만 걸러내고 나머지는 그대로 발행한다.
    // out은 직전 send()에서 큐로 move된 상태라 capacity가 0 이다.
    // -- clear()가 유지할 capacity 자체가 없으므로 이어지는 reserve가 매번 새로 할당한다.
    out.blurs.clear();
    out.blurs.reserve(in.blurs.size());
    for (const auto& blur : in.blurs) {
        if (isValidBlurTarget(blur)) {
            out.blurs.push_back(blur);
        } else {
            recordDrop("invalid blur target skipped");
        }
    }
    return true;
}

std::string MqttBlurSink::describe(const veda::BlurFrame& frame) const {
    return "ch=" + std::to_string(frame.ch) + " blurs=" + std::to_string(frame.blurs.size());
}