#pragma once

#include <memory>

class MqttTransport;

class MqttTransportFactory {
public:
    virtual ~MqttTransportFactory() = default;
    virtual std::unique_ptr<MqttTransport> create() const = 0;
};
