#include "network/DemoDeviceStatusGatewayFactory.h"

#include "network/DemoDeviceStatusGateway.h"

/**
 * @brief         demo 장비 상태 gateway를 생성합니다.
 * @param parent  Qt 객체 소유권을 연결할 부모 객체
 * @return        생성된 장비 상태 gateway
 */
std::shared_ptr<DeviceStatusGateway> DemoDeviceStatusGatewayFactory::create(QObject* parent) const {
    return std::make_shared<DemoDeviceStatusGateway>(parent);
}
