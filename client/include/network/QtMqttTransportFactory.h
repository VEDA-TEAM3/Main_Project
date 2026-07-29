#pragma once

#include "network/MqttConnectionConfig.h"
#include "network/MqttTransportFactory.h"

class QtMqttTransportFactory final : public MqttTransportFactory {
public:
    explicit QtMqttTransportFactory(MqttConnectionConfig config);
    std::unique_ptr<MqttTransport> create() const override;

private:
    MqttConnectionConfig config_;
};
