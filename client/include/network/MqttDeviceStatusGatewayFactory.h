#pragma once

#include "network/DeviceStatusGatewayFactory.h"
#include "network/MqttRuntimeConfig.h"

class MqttDeviceStatusGatewayFactory final : public DeviceStatusGatewayFactory {
public:
    explicit MqttDeviceStatusGatewayFactory(MqttRuntimeConfig config);
    std::shared_ptr<DeviceStatusGateway> create(QObject* parent = nullptr) const override;

private:
    MqttRuntimeConfig config_;
};
