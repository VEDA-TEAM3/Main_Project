#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QVector>
#include <memory>

#include "model/DigitalTwinMapDisplaySettings.h"
#include "model/StreamConfig.h"
#include "video/VideoRuntimeConfig.h"

class ClickableVideoWidget;
class DashboardPanelCoordinator;
class DashboardPanelFactory;
class DeviceStatusGatewayFactory;
class DeviceStatusPanel;
class DeviceStatusService;
class EventLogPanel;
class ObjectListPanel;
class QEvent;
class QFrame;
class QLabel;
class MapSettingsDialog;
class QResizeEvent;
class QShowEvent;
class ReportConfirmationDialog;
class ReportGateway;
class ReportSuccessDialog;
class StreamReceiverFactory;
class StreamSessionManager;
class VideoRiskBorderFrame;
class QWidget;
enum class DigitalTwinRiskLevel;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(std::shared_ptr<StreamReceiverFactory> streamReceiverFactory,
                        std::shared_ptr<DeviceStatusGatewayFactory> deviceStatusGatewayFactory,
                        std::shared_ptr<DashboardPanelFactory> dashboardPanelFactory,
                        std::shared_ptr<ReportGateway> reportGateway, VideoRuntimeConfig videoConfig,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void setupDashboardLayout();
    void setupDashboardPanels();
    void setupDashboardPanelCoordinator();
    void setupDeviceStatusService();
    void setupTopBarStatuses();
    void setupClock();
    void setupStreamSessionManager(std::shared_ptr<StreamReceiverFactory> receiverFactory);
    void setupVideoViewEvents();
    void setupReportActions();
    void openMapSettingsDialog();
    void openReportConfirmationDialog(int channelNumber);
    void openReportSuccessDialog(int channelNumber);
    void sendReport(int channelNumber);
    void handleReportFailure(int channelNumber, const QString& error);
    void setReportButtonsEnabled(bool enabled);
    QString reportRiskLevel(int channelNumber) const;

    void updateDashboardAdaptiveSizes();
    void updateSystemStatus(bool connected);
    void updateStreamConnectionStatus();
    void updateVideoRiskBorders(const QVector<DigitalTwinRiskLevel>& riskLevels);
    void updateCurrentDateTime();
    void setTopBarStatus(QLabel* label, const QString& title, const QString& status, const QString& color);

    void toggleExpandVideo(QWidget* targetWidget);
    void expandVideo(QWidget* targetWidget);
    void restoreVideoGrid();

private:
    std::shared_ptr<Ui::MainWindow> ui_;
    std::shared_ptr<DeviceStatusGatewayFactory> deviceStatusGatewayFactory_;
    std::shared_ptr<DashboardPanelFactory> dashboardPanelFactory_;
    std::shared_ptr<ReportGateway> reportGateway_;
    std::shared_ptr<DeviceStatusService> deviceStatusService_;
    VideoRuntimeConfig videoConfig_;

    QVector<QWidget*> videoWidgets_;
    QVector<VideoRiskBorderFrame*> videoTileFrames_;
    QVector<StreamConfig> streamConfigs_;
    QVector<bool> streamChannelReady_;

    StreamSessionManager* streamSessionManager_ = nullptr;
    DashboardPanelCoordinator* dashboardPanelCoordinator_ = nullptr;
    DeviceStatusPanel* deviceStatusPanel_ = nullptr;
    EventLogPanel* eventLogPanel_ = nullptr;
    ObjectListPanel* objectListPanel_ = nullptr;
    MapSettingsDialog* mapSettingsDialog_ = nullptr;
    ReportConfirmationDialog* reportConfirmationDialog_ = nullptr;
    ReportSuccessDialog* reportSuccessDialog_ = nullptr;
    QWidget* expandedWidget_ = nullptr;
    QTimer clockTimer_;
    DigitalTwinMapDisplaySettings mapDisplaySettings_;
    QVector<DigitalTwinRiskLevel> latestVideoRiskLevels_;

    bool streamSessionStarted_ = false;
    bool videoRiskBordersEnabled_ = true;
    bool faceBlurEnabled_ = true;
    bool licensePlateBlurEnabled_ = true;
    bool reportInProgress_ = false;
};
