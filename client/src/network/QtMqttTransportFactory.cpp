#include "network/QtMqttTransportFactory.h"

#include <utility>

#include "network/QtMqttTransport.h"

/**
 * @brief         Qt MQTT 전송 객체 factory를 생성합니다.
 * @param config  모든 전송 객체에 적용할 TLS 접속 설정
 */
QtMqttTransportFactory::QtMqttTransportFactory(MqttConnectionConfig config) : config_(std::move(config)) {}

/**
 * @brief   호출한 worker thread에서 Qt MQTT 전송 객체를 생성합니다.
 * @return  단독 수명으로 관리할 MQTT 전송 객체
 */
std::unique_ptr<MqttTransport> QtMqttTransportFactory::create() const {
    return std::make_unique<QtMqttTransport>(config_);
}
