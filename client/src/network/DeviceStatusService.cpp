#include "network/DeviceStatusService.h"

#include <QMetaObject>
#include <QMetaType>
#include <QThread>
#include <optional>
#include <utility>

#include "network/DeviceStatusGateway.h"
#include "network/DeviceStatusGatewayFactory.h"
#include "network/LatestBlurFrameBuffer.h"
#include "network/LatestRiskFrameBuffer.h"

namespace {
constexpr int statusServiceChannelCount = 4;
constexpr int maximumRecentReportKeys = 128;
constexpr int uiFlushIntervalMsec = 50;
}  // namespace

/**
 * @brief                 장비 상태 service를 생성하고 UI 갱신 병합기를 준비합니다.
 * @param gatewayFactory  실제 MQTT 또는 demo gateway 생성 factory
 * @param parent          Qt 객체 소유권을 연결할 부모 객체
 */
DeviceStatusService::DeviceStatusService(std::shared_ptr<DeviceStatusGatewayFactory> gatewayFactory, QObject* parent)
    : DeviceStatusService(std::move(gatewayFactory), std::make_shared<LatestBlurFrameBuffer>(),
                          std::make_shared<LatestRiskFrameBuffer>(), parent) {}

/**
 * @brief                  장비 상태 service를 교체 가능한 실시간 프레임 버퍼와 함께 생성합니다.
 * @param gatewayFactory   실제 MQTT 또는 demo gateway 생성 factory
 * @param blurFrameBuffer  채널별 최신 블러 프레임 버퍼
 * @param riskFrameBuffer  최신 위험 프레임 버퍼
 * @param parent           Qt 객체 소유권을 연결할 부모 객체
 */
DeviceStatusService::DeviceStatusService(std::shared_ptr<DeviceStatusGatewayFactory> gatewayFactory,
                                         std::shared_ptr<BlurFrameBuffer> blurFrameBuffer,
                                         std::shared_ptr<RiskFrameBuffer> riskFrameBuffer, QObject* parent)
    : QObject(parent),
      gatewayFactory_(std::move(gatewayFactory)),
      blurFrameBuffer_(blurFrameBuffer ? std::move(blurFrameBuffer) : std::make_shared<LatestBlurFrameBuffer>()),
      riskFrameBuffer_(riskFrameBuffer ? std::move(riskFrameBuffer) : std::make_shared<LatestRiskFrameBuffer>()) {
    qRegisterMetaType<DeviceOutputState>("DeviceOutputState");
    qRegisterMetaType<DeviceStatusReport>("DeviceStatusReport");
    qRegisterMetaType<DeviceChannelStatus>("DeviceChannelStatus");
    qRegisterMetaType<QVector<DeviceChannelStatus>>("QVector<DeviceChannelStatus>");
    qRegisterMetaType<RiskFrameData>("RiskFrameData");
    qRegisterMetaType<BlurRegionData>("BlurRegionData");
    qRegisterMetaType<BlurFrameData>("BlurFrameData");
    qRegisterMetaType<CentralEventData>("CentralEventData");

    uiFlushTimer_.setInterval(uiFlushIntervalMsec);
    uiFlushTimer_.setSingleShot(true);
    uiFlushTimer_.setTimerType(Qt::CoarseTimer);
    connect(&uiFlushTimer_, &QTimer::timeout, this, &DeviceStatusService::flushPendingStatuses);
}

/**
 * @brief   장비 상태 gateway와 worker thread를 종료합니다.
 */
DeviceStatusService::~DeviceStatusService() { stop(); }

/**
 * @brief   gateway worker thread를 구성하고 상태 수신을 시작합니다.
 */
void DeviceStatusService::start() {
    if (gatewayThread_) {
        return;
    }

    setupGateway();

    if (gateway_ && gatewayThread_) {
        gatewayThread_->start();
    }
}

/**
 * @brief   gateway 수신을 중지하고 worker thread를 안전하게 정리합니다.
 */
void DeviceStatusService::stop() {
    uiFlushTimer_.stop();
    pendingStatuses_.clear();
    blurFrameBuffer_->clear();
    riskFrameBuffer_->clear();

    if (!gatewayThread_) {
        return;
    }

    if (gateway_ && gatewayThread_->isRunning()) {
        DeviceStatusGateway* gateway = gateway_.get();
        QThread* serviceThread = thread();
        const bool stopped = QMetaObject::invokeMethod(
            gateway,
            [gateway, serviceThread]() {
                gateway->stop();
                gateway->moveToThread(serviceThread);
            },
            Qt::BlockingQueuedConnection);

        if (!stopped) {
            qWarning() << "[DeviceStatusService] Failed to stop gateway in its worker thread";
        }
    }

    gatewayThread_->quit();
    gatewayThread_->wait();
    gateway_.reset();
    gatewayThread_.reset();
    blurFrameBuffer_->clear();
    riskFrameBuffer_->clear();
}

/**
 * @brief   gateway를 전용 스레드로 이동하고 도메인 보고 signal을 연결합니다.
 */
void DeviceStatusService::setupGateway() {
    if (!gatewayFactory_) {
        return;
    }

    gatewayThread_ = std::make_shared<QThread>();
    gateway_ = gatewayFactory_->create();

    if (!gateway_) {
        gatewayThread_.reset();
        return;
    }

    if (!gateway_->moveToThread(gatewayThread_.get())) {
        qWarning() << "[DeviceStatusService] Failed to move gateway to worker thread";
        gateway_.reset();
        gatewayThread_.reset();
        return;
    }

    connect(gatewayThread_.get(), &QThread::started, gateway_.get(), &DeviceStatusGateway::start);
    connect(gateway_.get(), &DeviceStatusGateway::reportReceived, this, &DeviceStatusService::handleReport,
            Qt::QueuedConnection);
    connect(
        gateway_.get(), &DeviceStatusGateway::riskFrameReceived, this,
        [this](RiskFrameData frame) { queueRiskFrame(std::move(frame)); }, Qt::DirectConnection);
    connect(
        gateway_.get(), &DeviceStatusGateway::blurFrameReceived, this,
        [this](BlurFrameData frame) { queueBlurFrame(std::move(frame)); }, Qt::DirectConnection);
    connect(gateway_.get(), &DeviceStatusGateway::centralEventReceived, this,
            &DeviceStatusService::centralEventReceived, Qt::QueuedConnection);
    connect(gateway_.get(), &DeviceStatusGateway::brokerConnectionChanged, this,
            &DeviceStatusService::handleBrokerConnection, Qt::QueuedConnection);
}

/**
 * @brief        MQTT worker에서 받은 블러 프레임을 최신값 버퍼에 병합합니다.
 * @param frame  채널과 UTC timestamp 검증을 마친 블러 프레임
 */
void DeviceStatusService::queueBlurFrame(BlurFrameData frame) {
    if (!blurFrameBuffer_->submit(std::move(frame))) {
        return;
    }

    const bool invoked = QMetaObject::invokeMethod(this, [this]() { flushPendingBlurFrames(); }, Qt::QueuedConnection);
    if (!invoked) {
        blurFrameBuffer_->cancelPendingDelivery();
        qWarning() << "[DeviceStatusService] Failed to schedule blur metadata delivery";
    }
}

/**
 * @brief UI 이벤트 루프에는 채널별 최신 블러 프레임만 전달합니다.
 */
void DeviceStatusService::flushPendingBlurFrames() {
    QVector<BlurFrameData> frames = blurFrameBuffer_->takeLatestFrames();
    for (BlurFrameData& frame : frames) {
        emit blurFrameReceived(std::move(frame));
    }
}

/**
 * @brief        MQTT 작업 스레드에서 받은 위험 프레임을 최신값 버퍼에 병합합니다.
 * @param frame  timestamp 정렬과 계약 검증을 통과한 위험 프레임
 */
void DeviceStatusService::queueRiskFrame(RiskFrameData frame) {
    if (!riskFrameBuffer_->submit(std::move(frame))) {
        return;
    }

    const bool invoked = QMetaObject::invokeMethod(this, [this]() { flushPendingRiskFrame(); }, Qt::QueuedConnection);
    if (!invoked) {
        riskFrameBuffer_->cancelPendingDelivery();
        qWarning() << "[DeviceStatusService] Failed to schedule risk metadata delivery";
    }
}

/** @brief UI 이벤트 루프에는 대기 중인 가장 최신 위험 프레임 하나만 전달합니다. */
void DeviceStatusService::flushPendingRiskFrame() {
    std::optional<RiskFrameData> frame = riskFrameBuffer_->takeLatestFrame();
    if (frame.has_value()) {
        emit riskFrameReceived(std::move(*frame));
    }
}

void DeviceStatusService::handleBrokerConnection(bool connected) {
    emit brokerConnectionChanged(connected);

    if (connected) {
        return;
    }

    for (int channelIndex = 0; channelIndex < statusServiceChannelCount; ++channelIndex) {
        DeviceChannelStatus status = channelStatuses_.value(channelIndex);
        status.channelIndex = channelIndex;
        status.sensorHealth = SensorHealth::Unknown;
        status.feedbackHealth = DeviceFeedbackHealth::Unknown;
        status.sensorDetail = QStringLiteral("broker_disconnected");
        channelStatuses_.insert(channelIndex, status);
        queueChannelStatus(std::move(status));
    }
}

/**
 * @brief         gateway가 변환한 상태 보고를 종류별 정책에 따라 처리합니다.
 * @param report  수신된 controller 또는 HW 피드백 보고
 */
void DeviceStatusService::handleReport(DeviceStatusReport report) {
    if (isDuplicateReport(report)) {
        return;
    }

    switch (report.type) {
        case DeviceStatusReportType::ChannelStatusSnapshot:
            handleChannelStatusSnapshot(report);
            return;
        case DeviceStatusReportType::ControllerOnline:
            emit controllerOnlineChanged(true, report.node);
            return;
        case DeviceStatusReportType::SensorOnline:
            handleSensorHealth(report, SensorHealth::Online);
            return;
        case DeviceStatusReportType::SensorOffline:
            handleSensorHealth(report, SensorHealth::Offline);
            return;
        case DeviceStatusReportType::FeedbackConfirmed:
            handleConfirmedFeedback(report);
            return;
        case DeviceStatusReportType::FeedbackAcknowledged:
            handleAcknowledgedFeedback(report);
            return;
        case DeviceStatusReportType::FeedbackFailed:
            handleFailedFeedback(report);
            return;
        case DeviceStatusReportType::ProtocolError:
            emit protocolError(report.detail);
            return;
    }
}

/**
 * @brief         채널 상태 스냅샷에서 HW 연결 상태와 유효한 출력 상태를 함께 반영합니다.
 * @param report  shared/Contract.h의 ChannelStatus 규약으로 검증된 보고
 */
void DeviceStatusService::handleChannelStatusSnapshot(const DeviceStatusReport& report) {
    if (report.channelIndex < 0 || report.channelIndex >= statusServiceChannelCount) {
        emit protocolError(QStringLiteral("Invalid channel status snapshot channel"));
        return;
    }

    DeviceChannelStatus status = channelStatuses_.value(report.channelIndex);
    status.channelIndex = report.channelIndex;
    status.sensorHealth = report.hardwareAlive ? SensorHealth::Online : SensorHealth::Offline;
    status.sensorDetail =
        report.hardwareAlive ? QStringLiteral("hardware_alive") : QStringLiteral("hardware_unavailable");
    status.detail = status.sensorDetail;

    if (report.hardwareAlive && report.hasOutputState) {
        status.outputs = report.outputs;
        status.hasConfirmedState = true;
        status.feedbackHealth = DeviceFeedbackHealth::Confirmed;
        status.confirmedSourceTimestamp = report.sourceTimestamp;
    } else {
        status.feedbackHealth = DeviceFeedbackHealth::Unknown;
    }

    channelStatuses_.insert(status.channelIndex, status);
    queueChannelStatus(std::move(status));
}

void DeviceStatusService::handleSensorHealth(const DeviceStatusReport& report, SensorHealth health) {
    if (report.channelIndex < 0 || report.channelIndex >= statusServiceChannelCount) {
        emit protocolError(QStringLiteral("Invalid sensor health channel"));
        return;
    }

    DeviceChannelStatus status = channelStatuses_.value(report.channelIndex);
    status.channelIndex = report.channelIndex;
    status.sensorHealth = health;
    status.sensorDetail = report.detail;

    channelStatuses_.insert(status.channelIndex, status);
    queueChannelStatus(std::move(status));
}

void DeviceStatusService::handleAcknowledgedFeedback(const DeviceStatusReport& report) {
    if (report.channelIndex < 0 || report.channelIndex >= statusServiceChannelCount) {
        emit protocolError(QStringLiteral("Invalid acknowledged device feedback channel"));
        return;
    }

    DeviceChannelStatus status = channelStatuses_.value(report.channelIndex);
    status.channelIndex = report.channelIndex;
    status.detail = report.detail;

    channelStatuses_.insert(status.channelIndex, status);
    queueChannelStatus(std::move(status));
}

/**
 * @brief         성공적으로 확인된 실제 HW 출력만 마지막 확정 상태로 저장합니다.
 * @param report  state가 검증된 성공 피드백 보고
 */
void DeviceStatusService::handleConfirmedFeedback(const DeviceStatusReport& report) {
    if (report.channelIndex < 0 || report.channelIndex >= statusServiceChannelCount || !report.hasOutputState) {
        emit protocolError(QStringLiteral("Invalid confirmed device feedback"));
        return;
    }

    DeviceChannelStatus status = channelStatuses_.value(report.channelIndex);
    status.channelIndex = report.channelIndex;
    status.outputs = report.outputs;
    status.hasConfirmedState = true;
    status.feedbackHealth = DeviceFeedbackHealth::Confirmed;
    status.detail = report.detail;
    status.confirmedSourceTimestamp = report.sourceTimestamp;

    channelStatuses_.insert(status.channelIndex, status);
    queueChannelStatus(std::move(status));
}

/**
 * @brief         실패 Payload의 state를 무시하고 마지막 확정 출력 상태를 유지합니다.
 * @param report  UART timeout 등 상태 확인 실패 보고
 */
void DeviceStatusService::handleFailedFeedback(const DeviceStatusReport& report) {
    if (report.channelIndex < 0 || report.channelIndex >= statusServiceChannelCount) {
        emit protocolError(QStringLiteral("Invalid failed device feedback channel"));
        return;
    }

    DeviceChannelStatus status = channelStatuses_.value(report.channelIndex);
    status.channelIndex = report.channelIndex;
    status.feedbackHealth = DeviceFeedbackHealth::Failed;
    status.detail = report.detail;

    channelStatuses_.insert(status.channelIndex, status);
    queueChannelStatus(std::move(status));
    emit feedbackFailed(report.channelIndex, report.detail);
}

/**
 * @brief         채널별 최신 상태를 UI 반영 대기열에 저장합니다.
 * @param status  UI에 표시할 출력 및 피드백 상태
 */
void DeviceStatusService::queueChannelStatus(DeviceChannelStatus status) {
    pendingStatuses_.insert(status.channelIndex, std::move(status));
    scheduleUiFlush();
}

/**
 * @brief   아직 예약되지 않은 경우에만 UI 병합 갱신을 예약합니다.
 */
void DeviceStatusService::scheduleUiFlush() {
    if (!pendingStatuses_.isEmpty() && !uiFlushTimer_.isActive()) {
        uiFlushTimer_.start();
    }
}

/**
 * @brief   채널별 최신 상태를 하나의 UI frame으로 묶어 전달합니다.
 */
void DeviceStatusService::flushPendingStatuses() {
    if (pendingStatuses_.isEmpty()) {
        return;
    }

    QVector<DeviceChannelStatus> statuses;
    statuses.reserve(pendingStatuses_.size());

    for (auto iterator = pendingStatuses_.cbegin(); iterator != pendingStatuses_.cend(); ++iterator) {
        statuses.append(iterator.value());
    }

    pendingStatuses_.clear();
    emit channelStatusesReceived(std::move(statuses));
}

/**
 * @brief         QoS 1 재전송으로 이미 처리한 보고인지 확인합니다.
 * @param report  중복 여부를 검사할 상태 보고
 * @return        최근 처리 key와 동일하면 true
 */
bool DeviceStatusService::isDuplicateReport(const DeviceStatusReport& report) {
    const QString key = reportKey(report);

    if (key.isEmpty()) {
        return false;
    }

    if (recentReportKeys_.contains(key)) {
        return true;
    }

    rememberReportKey(key);
    return false;
}

/**
 * @brief         수정할 수 없는 Payload 필드 조합으로 중복 판별 key를 생성합니다.
 * @param report  key를 만들 상태 보고
 * @return        timestamp가 없으면 빈 문자열, 있으면 bounded cache용 key
 */
QString DeviceStatusService::reportKey(const DeviceStatusReport& report) const {
    if (report.sourceTimestamp <= 0) {
        return {};
    }

    return QStringLiteral("%1|%2|%3|%4|%5")
        .arg(report.node)
        .arg(report.channelIndex)
        .arg(report.sourceTimestamp)
        .arg(static_cast<int>(report.type))
        .arg(report.detail);
}

/**
 * @brief      최근 중복 판별 key를 제한 개수만 보관합니다.
 * @param key  새로 처리한 보고 key
 */
void DeviceStatusService::rememberReportKey(QString key) {
    recentReportKeys_.insert(key);
    reportKeyOrder_.enqueue(std::move(key));

    while (reportKeyOrder_.size() > maximumRecentReportKeys) {
        recentReportKeys_.remove(reportKeyOrder_.dequeue());
    }
}
