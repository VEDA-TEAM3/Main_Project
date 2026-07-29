#pragma once

#include "network/DeviceStatusGateway.h"

class QTimer;

class DemoDeviceStatusGateway final : public DeviceStatusGateway {
    Q_OBJECT

public:
    explicit DemoDeviceStatusGateway(QObject* parent = nullptr);

    void start() override;
    void stop() override;

private:
    DeviceStatusReport createRandomReport(int channelIndex) const;
    void publishControllerOnline();
    void publishNextFrame();

    QTimer* timer_ = nullptr;
};
