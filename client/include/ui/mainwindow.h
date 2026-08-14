#pragma once

#include <QMainWindow>
#include <QString>
#include <QTimer>
#include <QVector>
#include <memory>

#include "model/DigitalTwinMapDisplaySettings.h"
#include "model/DigitalTwinRuntimeConfig.h"
#include "model/StreamConfig.h"
#include "model/VideoPreprocessingSettings.h"
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
class QGridLayout;
class QLabel;
class QBoxLayout;
class QQuickWidget;
class MapSettingsDialog;
class QResizeEvent;
class QShowEvent;
class ReportGateway;
class StreamReceiverFactory;
class StreamSessionManager;
class VideoRiskBorderFrame;
class QWidget;
class QVariant;
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
                        DigitalTwinRuntimeConfig digitalTwinConfig, QString configSourcePath,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void setupDashboardLayout();
    void setupQuickTopBar();
    void setupQuickDashboardChrome();
    void setupQuickPanelHeaders();
    void setupQuickDialogOverlay();
    void addQuickPanelHeader(QBoxLayout* layout, const QString& title);
    QQuickWidget* createQuickView(const QString& qmlFile, QWidget* parent, Qt::WindowFlags windowFlags = {});
    void setupDashboardPanels();
    void setupDashboardPanelCoordinator();
    void setupDeviceStatusService();
    void setupTopBarStatuses();
    void setupClock();
    void setupWindowShortcuts();
    void setupStreamSessionManager(std::shared_ptr<StreamReceiverFactory> receiverFactory);
    void setupVideoViewEvents();
    void setupVideoAreaSelector();
    void setupReportActions();

private slots:
    void openMapSettingsDialog();
    void openVideoAreaSelectionDialog();
    void openReportConfirmationDialog(int channelNumber);
    void handleQuickDialogAccepted(int selectedIndex);
    void closeQuickDialog();

private:
    void openReportSuccessDialog(int channelNumber);
    void sendReport(int channelNumber);
    void handleReportFailure(int channelNumber, const QString& error);
    void setReportButtonsEnabled(bool enabled);
    QString reportRiskLevel(int channelNumber) const;
    int reportChannelNumberForSlot(int slotIndex) const;
    int videoAreaIndexForChannel(int channelIndex) const;
    const QVector<int>& channelsForArea(int areaIndex) const;
    bool isChannelVisible(int channelIndex) const;
    void syncStreamPresentation();
    void switchVideoArea(int areaIndex);

    void updateDashboardAdaptiveSizes();
    void updateSystemStatus(bool connected);
    void updateStreamConnectionStatus();
    void updateVideoRiskBorders(const QVector<DigitalTwinRiskLevel>& riskLevels);
    void updateCurrentDateTime();
    void setQuickTopBarProperty(const char* name, const QVariant& value);
    void setQuickDialogProperty(const char* name, const QVariant& value);
    void showQuickDialog(const QString& mode, const QString& title, const QString& message);
    void setReportDialogChannelProperties(int channelNumber);
    QRect quickDialogHostGeometry() const;
    void toggleFullScreen();

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
    /// 구역 추가를 덧붙일 app_config.json 경로
    QString configSourcePath_;

    QVector<QWidget*> videoWidgets_;
    QVector<VideoRiskBorderFrame*> videoTileFrames_;
    QVector<QGridLayout*> videoAreaLayouts_;
    QVector<StreamConfig> streamConfigs_;
    QVector<bool> streamChannelReady_;

    StreamSessionManager* streamSessionManager_ = nullptr;
    DashboardPanelCoordinator* dashboardPanelCoordinator_ = nullptr;
    DeviceStatusPanel* deviceStatusPanel_ = nullptr;
    EventLogPanel* eventLogPanel_ = nullptr;
    ObjectListPanel* objectListPanel_ = nullptr;
    MapSettingsDialog* mapSettingsDialog_ = nullptr;
    QWidget* expandedWidget_ = nullptr;
    QTimer clockTimer_;
    QQuickWidget* quickTopBar_ = nullptr;
    QQuickWidget* quickCctvToolbar_ = nullptr;
    QQuickWidget* quickLegend_ = nullptr;
    QQuickWidget* quickDialogOverlay_ = nullptr;
    DigitalTwinMapDisplaySettings mapDisplaySettings_;
    QVector<VideoPreprocessingSettings> videoPreprocessingSettingsByChannel_;
    QVector<DigitalTwinRiskLevel> latestVideoRiskLevels_;
    int selectedPreprocessingChannelIndex_ = 0;
    int currentVideoAreaIndex_ = 0;
    int pendingReportChannelNumber_ = 1;

    enum class QuickDialogMode {
        None,
        AreaSelection,
        ReportConfirmation,
        ReportSuccess,
    };
    QuickDialogMode quickDialogMode_ = QuickDialogMode::None;

    bool streamSessionStarted_ = false;
    bool videoRiskBordersEnabled_ = true;
    bool faceBlurEnabled_ = true;
    bool licensePlateBlurEnabled_ = true;
    bool reportInProgress_ = false;
    Qt::WindowStates windowStateBeforeFullScreen_ = Qt::WindowNoState;
};
