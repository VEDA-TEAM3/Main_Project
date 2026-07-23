#include "core/Controller.h"

#include <string>

#include "Logger.h"

namespace {
constexpr const char* kIface = "Controller";
}  // namespace

Controller::Controller(std::shared_ptr<IChannelReceiver> receiver, std::shared_ptr<IFrameAggregator> aggregator,
                       std::shared_ptr<ILocalToWorldTransform> transform, std::shared_ptr<ICrossChannelFuser> fuser,
                       std::shared_ptr<IZoneMapper> zoneMapper, std::shared_ptr<IRiskPolicy> riskPolicy,
                       std::shared_ptr<IHwEventDispatcher> dispatcher, std::shared_ptr<ISink> sink, int channelCount)
    : receiver_(std::move(receiver)),
      aggregator_(std::move(aggregator)),
      transform_(std::move(transform)),
      fuser_(std::move(fuser)),
      zoneMapper_(std::move(zoneMapper)),
      riskPolicy_(std::move(riskPolicy)),
      dispatcher_(std::move(dispatcher)),
      sink_(std::move(sink)),
      channelCount_(channelCount > 0 ? channelCount : 1),
      channelAlive_(static_cast<std::size_t>(channelCount_), false),
      hardwareAlive_(static_cast<std::size_t>(channelCount_), false) {
    receiver_->setCallback([this](const veda::TopViewFrame& frame) { aggregator_->push(frame); });

    // 채널 생존 신호(LWT) 배선 -- 구독만 해놓고 버려지던 경로를 여기서 받는다
    receiver_->setAliveCallback(
        [this](veda::ChannelId channel, bool alive) { this->onChannelAlive(channel, alive, "MQTT"); });

    // STM32 하트비트(상행) 배선
    dispatcher_->setStatusCallback(
        [this](veda::ChannelId channel, bool alive) { this->onChannelAlive(channel, alive, "STM32"); });

    aggregator_->setCallback(
        [this](std::vector<veda::TopViewFrame> frames) { this->processPipeline(std::move(frames)); });
}

void Controller::onChannelAlive(veda::ChannelId channel, bool alive, const char* source) {
    if (channel < 0 || channel >= channelCount_) {
        logError(kIface, std::string(source) + " 생존 신호의 채널 " + std::to_string(channel) + " 이 범위 밖 - 무시");
        return;
    }

    const bool isMqtt = std::string(source) == "MQTT";
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(aliveMutex_);
        auto& state = isMqtt ? channelAlive_ : hardwareAlive_;
        const std::size_t idx = static_cast<std::size_t>(channel);
        changed = state[idx] != alive;
        state[idx] = alive;
    }

    // 전환될 때만 로그 -- retained 메시지나 하트비트가 반복돼도 도배되지 않음
    if (changed) {
        const std::string message =
            std::string(source) + " 채널 " + std::to_string(channel) + (alive ? " 복구됨(alive)" : " 끊김(dead)");
        if (alive)
            logSuccess(kIface, message);
        else
            logError(kIface, message);
    }
}

Controller::~Controller() { stop(); }

void Controller::start() { receiver_->start(); }

void Controller::stop() { receiver_->stop(); }

void Controller::processPipeline(std::vector<veda::TopViewFrame> frames) {
    if (frames.empty()) {
        return;
    }

    transform_->transform(frames);

    auto worldFrame = fuser_->fuse(frames);

    zoneMapper_->assign(worldFrame);

    auto riskEval = riskPolicy_->evaluate(worldFrame);

    dispatcher_->dispatch(riskEval);

    sink_->send(worldFrame);

    // 윈도우마다(기본 100ms = 초당 10회) 도는 정상 경로라 Debug
    // -- 문자열 조립까지 레벨로 걸러냄
    if (isLogEnabled(LogLevel::Debug)) {
        veda::RiskLevel maxLevel = veda::RiskLevel::None;
        for (const auto& zone : riskEval.zoneLevels) {
            if (zone.level > maxLevel)
                maxLevel = zone.level;
        }
        logDebug(kIface, std::to_string(frames.size()) + "채널 → 객체 " + std::to_string(worldFrame.objects.size()) +
                             "개 융합, 최고 위험도=" + std::string(veda::toString(maxLevel)));
    }
}