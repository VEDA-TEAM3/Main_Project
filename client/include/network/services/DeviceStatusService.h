#pragma once

#include <QMap>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>
#include <memory>

#include "model/DeviceStatus.h"
#include "model/DeviceStatusReport.h"
#include "model/MqttRealtimeData.h"

class DeviceStatusGateway;
class DeviceStatusGatewayFactory;
class BlurFrameBuffer;
class RiskFrameBuffer;
class QThread;

class DeviceStatusService final : public QObject {
    Q_OBJECT

public:
    explicit DeviceStatusService(std::shared_ptr<DeviceStatusGatewayFactory> gatewayFactory, QObject* parent = nullptr);
    DeviceStatusService(std::shared_ptr<DeviceStatusGatewayFactory> gatewayFactory,
                        std::shared_ptr<BlurFrameBuffer> blurFrameBuffer,
                        std::shared_ptr<RiskFrameBuffer> riskFrameBuffer, QObject* parent = nullptr);
    ~DeviceStatusService() override;

    void start();
    void stop();

signals:
    void channelStatusesReceived(QVector<DeviceChannelStatus> statuses);
    void riskFrameReceived(RiskFrameData frame);
    void blurFrameReceived(BlurFrameData frame);
    void centralEventReceived(CentralEventData event);
    void brokerConnectionChanged(bool connected);
    void controllerOnlineChanged(bool online, QString node);
    void feedbackFailed(int channelIndex, QString detail);
    void protocolError(QString detail);

private:
    void setupGateway();
    void queueBlurFrame(BlurFrameData frame);
    void flushPendingBlurFrames();
    void queueRiskFrame(RiskFrameData frame);
    void flushPendingRiskFrame();
    void handleBrokerConnection(bool connected);
    void handleReport(DeviceStatusReport report);
    void handleChannelStatusSnapshot(const DeviceStatusReport& report);
    void handleSensorHealth(const DeviceStatusReport& report, SensorHealth health);
    void handleConfirmedFeedback(const DeviceStatusReport& report);
    void handleAcknowledgedFeedback(const DeviceStatusReport& report);
    void handleFailedFeedback(const DeviceStatusReport& report);
    void queueChannelStatus(DeviceChannelStatus status);
    void scheduleUiFlush();
    void flushPendingStatuses();
    bool isDuplicateReport(const DeviceStatusReport& report);
    QString reportKey(const DeviceStatusReport& report) const;
    void rememberReportKey(QString key);

    std::shared_ptr<DeviceStatusGatewayFactory> gatewayFactory_;
    std::shared_ptr<BlurFrameBuffer> blurFrameBuffer_;
    std::shared_ptr<RiskFrameBuffer> riskFrameBuffer_;
    std::shared_ptr<QThread> gatewayThread_;
    std::shared_ptr<DeviceStatusGateway> gateway_;
    QMap<int, DeviceChannelStatus> channelStatuses_;
    QMap<int, DeviceChannelStatus> pendingStatuses_;
    QSet<QString> recentReportKeys_;
    QQueue<QString> reportKeyOrder_;
    QTimer uiFlushTimer_;
};
