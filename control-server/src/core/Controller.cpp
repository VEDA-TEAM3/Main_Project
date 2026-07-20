#include "core/Controller.h"

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
}