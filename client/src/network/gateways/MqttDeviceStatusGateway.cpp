#include "network/gateways/MqttDeviceStatusGateway.h"

#include <QDateTime>
#include <QDebug>
#include <utility>

#include "network/parsing/MqttPayloadLimits.h"
#include "network/realtime/BlurFrameDispatcher.h"
#include "network/realtime/LatestBlurFrameBuffer.h"
#include "network/realtime/RiskFrameDispatcher.h"
#include "network/routing/MqttMessageRouter.h"
#include "network/transport/MqttTransport.h"
#include "network/transport/MqttTransportFactory.h"

namespace {
/// 프로토콜 오류를 이 주기로 한 번만 올린다. 오류 하나마다 로그 한 줄과 thread 경계를
/// 넘는 signal이 하나씩 나가므로, 제한이 없으면 잘못된 메시지를 쏟아붓는 것만으로
/// GUI 이벤트 큐가 payload 크기 상한에 걸리기 한참 전에 밀린다
constexpr qint64 protocolErrorIntervalMsec = 30000;

QString riskLevelName(DigitalTwinRiskLevel riskLevel) {
    switch (riskLevel) {
        case DigitalTwinRiskLevel::Warning:
            return QStringLiteral("warning");
        case DigitalTwinRiskLevel::Danger:
            return QStringLiteral("danger");
        case DigitalTwinRiskLevel::Normal:
        default:
            return QStringLiteral("normal");
    }
}

QString debugPayloadText(const QByteArray& payload, qsizetype maximumLength) {
    QString text = QString::fromUtf8(payload).simplified();
    if (maximumLength > 0 && text.size() > maximumLength) {
        text = text.left(maximumLength) + QStringLiteral("...");
    }
    return text;
}
}  // namespace

/**
 * @brief                   MQTT gateway를 전송 구현과 토픽 router로 조립합니다.
 * @param transportFactory  worker thread에서 실제 transport를 생성할 factory
 * @param messageRouter     등록된 토픽 handler를 선택할 router
 * @param config            로그 카테고리 플래그를 포함한 MQTT 실행 설정
 * @param parent            Qt 객체 소유권을 연결할 부모 객체
 */
MqttDeviceStatusGateway::MqttDeviceStatusGateway(std::shared_ptr<MqttTransportFactory> transportFactory,
                                                 std::shared_ptr<MqttMessageRouter> messageRouter,
                                                 MqttRuntimeConfig config, QObject* parent)
    : DeviceStatusGateway(parent),
      transportFactory_(std::move(transportFactory)),
      messageRouter_(std::move(messageRouter)),
      lastBlurDebugLogMsec_(qMax(0, config.channelCount), 0),
      blurDebugLogIntervalMsec_(config.blurDebugLogIntervalMsec),
      riskDebugLogIntervalMsec_(config.riskDebugLogIntervalMsec),
      maximumDebugPayloadLength_(config.maximumDebugPayloadLength),
      logStatusPayload_(config.logStatusPayload),
      logRisk_(config.logRisk),
      logBlur_(config.logBlur) {
    blurDispatcher_ = new BlurFrameDispatcher(config.dispatcher, std::make_shared<LatestBlurFrameBuffer>(), this);
    connect(blurDispatcher_, &BlurFrameDispatcher::frameReady, this, &DeviceStatusGateway::blurFrameReceived);

    riskDispatcher_ = new RiskFrameDispatcher(config.dispatcher, this);
    connect(riskDispatcher_, &RiskFrameDispatcher::frameReady, this, &DeviceStatusGateway::riskFrameReceived);
}

MqttDeviceStatusGateway::~MqttDeviceStatusGateway() = default;

/**
 * @brief worker thread에서 transport를 생성하고 MQTT 수신을 시작합니다.
 */
void MqttDeviceStatusGateway::start() {
    if (transport_) {
        return;
    }

    if (!transportFactory_ || !messageRouter_) {
        emitProtocolError(QStringLiteral("MQTT gateway composition is incomplete"));
        return;
    }

    transport_ = transportFactory_->create();
    if (!transport_) {
        emitProtocolError(QStringLiteral("MQTT transport creation failed"));
        return;
    }

    lastBlurDebugLogMsec_.fill(0);
    lastRiskDebugLogMsec_ = 0;
    blurDispatcher_->start();
    riskDispatcher_->start();

    MqttTransportCallbacks callbacks;
    callbacks.connectionChanged = [this](bool connected) { handleConnectionChanged(connected); };
    callbacks.messageReceived = [this](const QByteArray& payload, const QString& topic) {
        handleMessage(payload, topic);
    };
    callbacks.errorOccurred = [this](QString detail) { emitProtocolError(std::move(detail)); };
    transport_->setCallbacks(std::move(callbacks));
    transport_->start();
}

/**
 * @brief dispatcher와 transport를 생성 thread에서 순서대로 정리합니다.
 */
void MqttDeviceStatusGateway::stop() {
    blurDispatcher_->stop();
    riskDispatcher_->stop();

    if (!transport_) {
        return;
    }

    transport_->stop();
    transport_.reset();
}

/** @brief broker 상태를 UI에 전달하고 연결 직후 구독을 등록합니다. */
void MqttDeviceStatusGateway::handleConnectionChanged(bool connected) {
    emit brokerConnectionChanged(connected);
    if (connected) {
        subscribeToTopics();
        return;
    }

    if (blurDispatcher_) {
        blurDispatcher_->reset();
    }
    if (riskDispatcher_) {
        riskDispatcher_->reset();
    }
}

/** @brief router에 등록된 handler의 구독 목록을 transport에 적용합니다. */
void MqttDeviceStatusGateway::subscribeToTopics() {
    if (!transport_ || !messageRouter_) {
        return;
    }

    for (const MqttSubscription& subscription : messageRouter_->subscriptions()) {
        transport_->subscribe(subscription);
    }
}

/**
 * @brief          MQTT 메시지를 토픽 handler로 변환한 뒤 기존 domain signal로
 * 전달합니다.
 * @param payload  MQTT payload
 * @param topic    실제 수신 토픽
 */
void MqttDeviceStatusGateway::handleMessage(const QByteArray& payload, const QString& topic) {
    if (!messageRouter_) {
        emitProtocolError(QStringLiteral("MQTT message router is unavailable"));
        return;
    }

    // JSON 파싱 전에 막는다. 한 번 파싱하고 나면 문서 전체가 이미 메모리에 올라와 있다
    if (payload.size() > maximumMqttPayloadBytes) {
        emitProtocolError(QStringLiteral("MQTT payload is too large on %1: %2 bytes").arg(topic).arg(payload.size()));
        return;
    }

    MqttRouteResult result = messageRouter_->route(payload, topic);
    if (logStatusPayload_ && result.logPayload) {
        logReceivedMessage(payload, topic);
    }

    if (!result.handled || !result.successful) {
        emitProtocolError(result.error.isEmpty() ? QStringLiteral("MQTT message routing failed: %1").arg(topic)
                                                 : std::move(result.error));
        return;
    }

    for (const BlurFrameData& frame : result.messages.blurFrames) {
        logBlurFrame(topic, frame);
    }
    for (const RiskFrameData& frame : result.messages.riskFrames) {
        logRiskFrame(topic, frame);
    }

    dispatchMessages(std::move(result.messages));
}

/** @brief 변환된 도메인 메시지를 서비스 signal 또는 최신값 dispatcher로
 * 전달합니다. */
void MqttDeviceStatusGateway::dispatchMessages(MqttMessageBatch messages) {
    for (DeviceStatusReport& report : messages.reports) {
        emit reportReceived(std::move(report));
    }
    for (CentralEventData& event : messages.centralEvents) {
        emit centralEventReceived(std::move(event));
    }
    for (RiskFrameData& frame : messages.riskFrames) {
        riskDispatcher_->submitFrame(std::move(frame));
    }
    for (BlurFrameData& frame : messages.blurFrames) {
        blurDispatcher_->submitFrame(std::move(frame));
    }
}

/** @brief 상태 및 이벤트 MQTT 메시지를 읽기 쉬운 제한 길이 텍스트로 출력합니다.
 */
void MqttDeviceStatusGateway::logReceivedMessage(const QByteArray& payload, const QString& topic) const {
    qInfo().noquote() << QStringLiteral("[MQTT RX] topic=%1 bytes=%2 payload=%3")
                             .arg(topic)
                             .arg(payload.size())
                             .arg(debugPayloadText(payload, maximumDebugPayloadLength_));
}

/** @brief 고빈도 블러 수신 상태를 채널별 제한 주기로 출력합니다. */
void MqttDeviceStatusGateway::logBlurFrame(const QString& topic, const BlurFrameData& frame) {
    if (!logBlur_ || frame.channelIndex < 0 || frame.channelIndex >= lastBlurDebugLogMsec_.size()) {
        return;
    }

    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
    qint64& lastLogMsec = lastBlurDebugLogMsec_[frame.channelIndex];
    if (blurDebugLogIntervalMsec_ <= 0 || (lastLogMsec > 0 && nowMsec - lastLogMsec < blurDebugLogIntervalMsec_)) {
        return;
    }

    lastLogMsec = nowMsec;
    qInfo().noquote() << QStringLiteral("[MQTT BLUR] topic=%1 channel=%2 ts=%3 regions=%4")
                             .arg(topic)
                             .arg(frame.channelIndex)
                             .arg(frame.sourceTimestamp)
                             .arg(frame.regions.size());
}

/** @brief 통합 위험 수신 상태를 설정된 주기로 제한하여 출력합니다. */
void MqttDeviceStatusGateway::logRiskFrame(const QString& topic, const RiskFrameData& frame) {
    if (!logRisk_) {
        return;
    }

    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
    if (riskDebugLogIntervalMsec_ > 0 && lastRiskDebugLogMsec_ > 0 &&
        nowMsec - lastRiskDebugLogMsec_ < riskDebugLogIntervalMsec_) {
        return;
    }

    lastRiskDebugLogMsec_ = nowMsec;
    qInfo().noquote() << QStringLiteral("[MQTT RISK] topic=%1 ts=%2 objects=%3")
                             .arg(topic)
                             .arg(frame.sourceTimestamp)
                             .arg(frame.objects.size());

    for (const RiskObjectData& object : frame.objects) {
        qInfo().noquote() << QStringLiteral("[MQTT RISK OBJECT] gid=%1 pos=(%2,%3) cls=%4 risk=%5 zoneId=%6")
                                 .arg(object.globalId)
                                 .arg(object.worldPosition.x(), 0, 'f', 2)
                                 .arg(object.worldPosition.y(), 0, 'f', 2)
                                 .arg(object.objectClass)
                                 .arg(riskLevelName(object.riskLevel))
                                 .arg(object.zoneId);
    }
}

/** @brief MQTT 계약 또는 전송 오류를 기존 상태 서비스 경로로 전달합니다. */
void MqttDeviceStatusGateway::emitProtocolError(QString detail) {
    // 오류 하나마다 GUI thread로 signal이 하나 간다. 잘못된 메시지에는 제한이 없으므로
    // 주기당 하나만 올리고 나머지는 개수만 세어 다음 오류에 붙인다
    const qint64 nowMsec = QDateTime::currentMSecsSinceEpoch();
    if (lastProtocolErrorMsec_ > 0 && nowMsec - lastProtocolErrorMsec_ < protocolErrorIntervalMsec) {
        ++suppressedProtocolErrorCount_;
        return;
    }

    if (suppressedProtocolErrorCount_ > 0) {
        detail += QStringLiteral(" (+%1 suppressed)").arg(suppressedProtocolErrorCount_);
        suppressedProtocolErrorCount_ = 0;
    }
    lastProtocolErrorMsec_ = nowMsec;

    // 오류는 로그 카테고리와 무관하게 항상 남긴다
    qWarning().noquote() << QStringLiteral("[MQTT ERROR] %1").arg(detail);

    DeviceStatusReport report;
    report.type = DeviceStatusReportType::ProtocolError;
    report.sourceTimestamp = QDateTime::currentMSecsSinceEpoch();
    report.detail = std::move(detail);
    emit reportReceived(std::move(report));
}
