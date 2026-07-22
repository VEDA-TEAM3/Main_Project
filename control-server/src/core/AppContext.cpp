#include "core/AppContext.h"

#include "aggregate/TimeWindowAggregatorV2.h"
#include "dispatch/ConsoleDispatcher.h"
#include "fuse/ConcatFuser.h"
#include "log/Logger.h"
#include "metric/EuclideanMetric.h"
#include "receive/MqttChannelReceiver.h"
#include "risk/ThresholdRiskPolicy.h"
#include "sink/MqttTransport.h"
#include "time/SystemClock.h"
#include "transform/NullTransform.h"
#include "zone/AngleZoneMapper.h"

namespace {
constexpr const char* kIface = "AppContext";
}  // namespace

AppContext::AppContext(const AppConfig& config) : config_(config) {}

std::shared_ptr<Controller> AppContext::buildController() {
    auto clock = std::make_shared<SystemClock>();
    auto metric = std::make_shared<EuclideanMetric>();

    auto receiver = std::make_shared<MqttChannelReceiver>();
    auto aggregator = std::make_shared<TimeWindowAggregatorV2>(clock, config_.windowSizeMs, config_.channelCount);
    auto transform = std::make_shared<NullTransform>();
    auto fuser = std::make_shared<ConcatFuser>(metric, config_.risk.dedupMergeDistance);
    auto zoneMapper = std::make_shared<AngleZoneMapper>(config_.zoneBoundaries);
    auto riskPolicy = std::make_shared<ThresholdRiskPolicy>(metric, config_.risk.warningDistance,
                                                            config_.risk.dangerousDistance, config_.channelCount);
    auto dispatcher = std::make_shared<ConsoleDispatcher>();
    auto sink = std::make_shared<MqttTransport>(config_);

    logSuccess(kIface,
               "파이프라인 조립 완료 (receiver=NullReceiver, transform=NullTransform, "
               "zoneMapper=AngleZoneMapper, sink=ConsoleSink)");

    return std::make_shared<Controller>(receiver, aggregator, transform, fuser, zoneMapper, riskPolicy, dispatcher,
                                        sink);
}