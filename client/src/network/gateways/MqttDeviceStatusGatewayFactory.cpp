#include "network/gateways/MqttDeviceStatusGatewayFactory.h"

#include <QVector>
#include <memory>

#include "network/gateways/MqttDeviceStatusGateway.h"
#include "network/routing/BlurTopicHandler.h"
#include "network/routing/DeviceStatusTopicHandler.h"
#include "network/routing/MqttMessageRouter.h"
#include "network/routing/RiskTopicHandler.h"
#include "network/transport/MqttConnectionConfig.h"
#include "network/transport/QtMqttTransportFactory.h"

MqttDeviceStatusGatewayFactory::MqttDeviceStatusGatewayFactory(MqttRuntimeConfig config) : config_(std::move(config)) {}

std::shared_ptr<DeviceStatusGateway> MqttDeviceStatusGatewayFactory::create(QObject* parent) const {
    auto transportFactory = std::make_shared<QtMqttTransportFactory>(config_.connection);

    QVector<std::shared_ptr<MqttTopicHandler>> handlers;
    handlers.append(std::make_shared<DeviceStatusTopicHandler>(config_.topics, config_.channelCount));
    handlers.append(std::make_shared<RiskTopicHandler>(config_.topics.risk, config_.channelCount));
    handlers.append(std::make_shared<BlurTopicHandler>(config_.topics.blur, config_.channelCount));
    auto messageRouter = std::make_shared<MqttMessageRouter>(std::move(handlers));

    return std::make_shared<MqttDeviceStatusGateway>(std::move(transportFactory), std::move(messageRouter), config_,
                                                     parent);
}
