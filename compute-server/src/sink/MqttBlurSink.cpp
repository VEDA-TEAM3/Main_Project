#include "sink/MqttBlurSink.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr const char* kIface = "MqttBlur";
}  // namespace

MqttBlurSink::MqttBlurSink(std::shared_ptr<IMqttTransport> transport, const AppConfig& config)
    : MqttFrameSink<veda::BlurFrame>(std::move(transport), veda::topic::blur(config.channelId), veda::qos::kBlur,
                                     static_cast<std::size_t>(std::max(1, config.mqttBlurMaxQueueSize)), kIface),
      channelCount_(config.channelCount) {}

MqttBlurSink::~MqttBlurSink() { shutdown(); }

bool MqttBlurSink::isValidFrame(const veda::BlurFrame& frame) const noexcept {
    if (frame.v != veda::kSchemaVersion)
        return false;

    if (frame.ts <= 0)
        return false;

    if (frame.ch < 0 || frame.ch >= channelCount_)
        return false;

    return true;
}

bool MqttBlurSink::isValidBlurTarget(const veda::BlurTarget& blur) const noexcept {
    if (!veda::isBlurClass(blur.cls))
        return false;

    const auto& box = blur.box;
    if (!std::isfinite(box.l) || !std::isfinite(box.t) || !std::isfinite(box.r) || !std::isfinite(box.b))
        return false;

    if (box.l < 0.0 || box.l > 1.0 || box.t < 0.0 || box.t > 1.0 || box.r < 0.0 || box.r > 1.0 || box.b < 0.0 ||
        box.b > 1.0)
        return false;

    if (box.l > box.r || box.t > box.b)
        return false;

    return true;
}

bool MqttBlurSink::prepare(const veda::BlurFrame& in, veda::BlurFrame& out) noexcept {
    if (!isValidFrame(in))
        return false;

    out.v = in.v;
    out.ts = in.ts;
    out.ch = in.ch;

    // 개별 blur 대상 중 클래스/좌표가 이상한 것만 걸러내고 나머지는 그대로 발행한다.
    // (얼굴 하나가 인식 안 되는 클래스라고 같은 프레임의 다른 blur까지 통째로 버리지 않기 위함
    // -- blurs가 비어 있는 프레임도 정상: 이전 프레임의 blur 영역을 지우려면 빈 프레임도 필요)
    out.blurs.clear();  // clear()는 capacity를 유지하므로 재할당이 없음
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
