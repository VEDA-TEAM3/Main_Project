#include "core/Controller.h"

#include <string>

#include "log/Logger.h"

namespace {
constexpr const char* kIface = "Controller";
}  // namespace

Controller::Controller(std::shared_ptr<IChannelReceiver> receiver, std::shared_ptr<IFrameAggregator> aggregator,
                       std::shared_ptr<ILocalToWorldTransform> transform, std::shared_ptr<ICrossChannelFuser> fuser,
                       std::shared_ptr<IZoneMapper> zoneMapper, std::shared_ptr<IRiskPolicy> riskPolicy,
                       std::shared_ptr<IHwEventDispatcher> dispatcher, std::shared_ptr<ISink> sink)
    : receiver_(std::move(receiver)),
      aggregator_(std::move(aggregator)),
      transform_(std::move(transform)),
      fuser_(std::move(fuser)),
      zoneMapper_(std::move(zoneMapper)),
      riskPolicy_(std::move(riskPolicy)),
      dispatcher_(std::move(dispatcher)),
      sink_(std::move(sink)) {
    receiver_->setCallback([this](const veda::TopViewFrame& frame) { aggregator_->push(frame); });

    aggregator_->setCallback(
        [this](std::vector<veda::TopViewFrame> frames) { this->processPipeline(std::move(frames)); });
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

    veda::RiskLevel maxLevel = veda::RiskLevel::None;
    for (const auto& zone : riskEval.zoneLevels) {
        if (zone.level > maxLevel)
            maxLevel = zone.level;
    }
    logSuccess(kIface, std::to_string(frames.size()) + "채널 → 객체 " + std::to_string(worldFrame.objects.size()) +
                           "개 융합, 최고 위험도=" + std::string(veda::toString(maxLevel)));
}