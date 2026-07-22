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
#include "transform/AffineLocalToWorldTransform.h"
#include "zone/AngleZoneMapper.h"

namespace {
constexpr const char* kIface = "AppContext";
}  // namespace

AppContext::AppContext(const AppConfig& config) : config_(config) {}

std::shared_ptr<Controller> AppContext::buildController() {
    auto clock = std::make_shared<SystemClock>();
    auto metric = std::make_shared<EuclideanMetric>();

    // receiver(수신)와 sink(발행)가 mosquitto 클라이언트/연결 하나를 공유함
    // -- 각자 별도 인스턴스를 만들면 TLS 연결이 두 개로 늘어나 지연시간과 리소스를 낭비함
    auto sink = std::make_shared<MqttTransport>(config_);
    auto receiver =
        std::make_shared<MqttChannelReceiver>(sink, config_.channelCount, config_.mqttReceiverRetryIntervalMs);
    auto aggregator = std::make_shared<TimeWindowAggregatorV2>(clock, config_.windowSizeMs, config_.channelCount);
    auto transform = std::make_shared<AffineLocalToWorldTransform>(config_.cameraCalibrations);
    auto fuser = std::make_shared<ConcatFuser>(metric, config_.risk.dedupMergeDistance);
    auto zoneMapper = std::make_shared<AngleZoneMapper>(config_.zoneBoundaries);
    auto riskPolicy = std::make_shared<ThresholdRiskPolicy>(metric, config_.risk.warningDistance,
                                                            config_.risk.dangerousDistance, config_.channelCount);
    auto dispatcher = std::make_shared<ConsoleDispatcher>();

    logSuccess(kIface,
               "파이프라인 조립 완료 (receiver=MqttChannelReceiver, transform=AffineLocalToWorldTransform, "
               "zoneMapper=AngleZoneMapper, sink=MqttTransport)");

    return std::make_shared<Controller>(receiver, aggregator, transform, fuser, zoneMapper, riskPolicy, dispatcher,
                                        sink);
}