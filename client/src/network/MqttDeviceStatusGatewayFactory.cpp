#include "network/MqttDeviceStatusGatewayFactory.h"

#include <QVector>
#include <memory>

#include "network/BlurTopicHandler.h"
#include "network/DeviceStatusTopicHandler.h"
#include "network/MqttConnectionConfig.h"
#include "network/MqttDeviceStatusGateway.h"
#include "network/MqttMessageRouter.h"
#include "network/QtMqttTransportFactory.h"
#include "network/RiskTopicHandler.h"

MqttDeviceStatusGatewayFactory::MqttDeviceStatusGatewayFactory(MqttRuntimeConfig config) : config_(std::move(config)) {}

std::shared_ptr<DeviceStatusGateway> MqttDeviceStatusGatewayFactory::create(QObject* parent) const {
    auto transportFactory = std::make_shared<QtMqttTransportFactory>(config_.connection);

    QVector<std::shared_ptr<MqttTopicHandler>> handlers;
    handlers.append(std::make_shared<DeviceStatusTopicHandler>(config_.topics));
    handlers.append(std::make_shared<RiskTopicHandler>(config_.topics.risk));
    handlers.append(std::make_shared<BlurTopicHandler>(config_.topics.blur));
    auto messageRouter = std::make_shared<MqttMessageRouter>(std::move(handlers));

    return std::make_shared<MqttDeviceStatusGateway>(std::move(transportFactory), std::move(messageRouter), config_,
                                                     parent);
}
