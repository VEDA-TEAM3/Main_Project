#pragma once

#include <memory>

class DeviceStatusGateway;
class QObject;

class DeviceStatusGatewayFactory {
public:
    virtual ~DeviceStatusGatewayFactory() = default;

    virtual std::shared_ptr<DeviceStatusGateway> create(QObject* parent = nullptr) const = 0;
};
