#include "network/DemoDeviceStatusGateway.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QTimer>

namespace {
constexpr int demoGatewayIntervalMsec = 5000;
constexpr int deviceChannelCount = 4;
constexpr int feedbackFailurePercent = 10;
constexpr int warningPercent = 25;
constexpr int dangerPercent = 10;

/**
 * @brief   실제 HW 피드백과 같은 독립 출력 구조의 demo 상태를 생성합니다.
 * @return  무작위 LED, 경광등, 부저 출력 상태
 */
DeviceOutputState createRandomOutputs() {
    DeviceOutputState outputs;
    const int ledRoll = QRandomGenerator::global()->bounded(100);

    if (ledRoll < dangerPercent) {
        outputs.ledRed = true;
    } else if (ledRoll < dangerPercent + warningPercent) {
        outputs.ledYellow = true;
    } else {
        outputs.ledGreen = true;
    }

    outputs.beacon = QRandomGenerator::global()->bounded(2) != 0;
    outputs.buzzer = QRandomGenerator::global()->bounded(2) != 0;
    return outputs;
}
}  // namespace

/**
 * @brief         실제 MQTT 장비 상태 수신 흐름을 모사하는 demo gateway를 생성합니다.
 * @param parent  Qt 객체 소유권을 연결할 부모 객체
 */
DemoDeviceStatusGateway::DemoDeviceStatusGateway(QObject* parent) : DeviceStatusGateway(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(demoGatewayIntervalMsec);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &DemoDeviceStatusGateway::publishNextFrame);
}

/**
 * @brief   demo broker 연결과 RPi 온라인 보고 후 상태 송출을 시작합니다.
 */
void DemoDeviceStatusGateway::start() {
    if (!timer_ || timer_->isActive()) {
        return;
    }

    emit brokerConnectionChanged(true);
    publishControllerOnline();
    publishNextFrame();
    timer_->start();
}

/**
 * @brief   demo 상태 송출을 중지하고 broker 연결 종료를 알립니다.
 */
void DemoDeviceStatusGateway::stop() {
    if (timer_) {
        timer_->stop();
    }

    emit brokerConnectionChanged(false);
}

/**
 * @brief               단일 채널의 성공 또는 실패 HW 피드백 보고를 생성합니다.
 * @param channelIndex  생성할 Qt 채널 index
 * @return              실제 MQTT parser 출력과 동일한 도메인 보고
 */
DeviceStatusReport DemoDeviceStatusGateway::createRandomReport(int channelIndex) const {
    DeviceStatusReport report;
    report.channelIndex = channelIndex;
    report.sourceTimestamp = QDateTime::currentMSecsSinceEpoch();
    report.node = QStringLiteral("rpi1");

    if (QRandomGenerator::global()->bounded(100) < feedbackFailurePercent) {
        report.type = DeviceStatusReportType::FeedbackFailed;
        report.detail = QStringLiteral("uart_response_timeout");
        return report;
    }

    report.type = DeviceStatusReportType::FeedbackConfirmed;
    report.detail = QStringLiteral("hardware_feedback_received");
    report.hasOutputState = true;
    report.outputs = createRandomOutputs();
    return report;
}

/**
 * @brief   state가 없는 실제 RPi 컨트롤러 온라인 메시지를 모사합니다.
 */
void DemoDeviceStatusGateway::publishControllerOnline() {
    DeviceStatusReport report;
    report.type = DeviceStatusReportType::ControllerOnline;
    report.channelIndex = 0;
    report.node = QStringLiteral("rpi1");
    report.detail = QStringLiteral("rpi_controller_online");
    emit reportReceived(std::move(report));
}

/**
 * @brief   4채널 각각을 독립 MQTT 메시지처럼 순차 송출합니다.
 */
void DemoDeviceStatusGateway::publishNextFrame() {
    for (int channelIndex = 0; channelIndex < deviceChannelCount; ++channelIndex) {
        emit reportReceived(createRandomReport(channelIndex));
    }
}
