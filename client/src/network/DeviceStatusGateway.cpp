#include "network/DeviceStatusGateway.h"

/**
 * @brief         장비 상태 gateway 공통 기반 객체를 생성합니다.
 * @param parent  Qt 객체 소유권을 연결할 부모 객체
 */
DeviceStatusGateway::DeviceStatusGateway(QObject* parent) : QObject(parent) {}

/**
 * @brief   장비 상태 gateway 공통 기반 객체를 해제합니다.
 */
DeviceStatusGateway::~DeviceStatusGateway() = default;
