#include "ui/mainwindow.h"

#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>
#include <algorithm>
#include <array>
#include <memory>
#include <utility>

#include "model/DigitalTwinTypes.h"
#include "network/DeviceStatusGatewayFactory.h"
#include "network/DeviceStatusService.h"
#include "network/ReportGateway.h"
#include "ui/ClickableVideoWidget.h"
#include "ui/DashboardLayout.h"
#include "ui/DigitalTwinMapWidget.h"
#include "ui/VideoRiskBorderFrame.h"
#include "ui/dialogs/MapSettingsDialog.h"
#include "ui/dialogs/ReportConfirmationDialog.h"
#include "ui/dialogs/ReportSuccessDialog.h"
#include "ui/panels/DashboardPanelCoordinator.h"
#include "ui/panels/DashboardPanelFactory.h"
#include "ui/panels/DeviceStatusPanel.h"
#include "ui/panels/EventLogPanel.h"
#include "ui/panels/ObjectListPanel.h"
#include "ui_mainwindow.h"
#include "video/StreamReceiverFactory.h"
#include "video/StreamSessionManager.h"

namespace {
constexpr int requiredCctvChannelCount = 4;
const QString normalStatusColor = QStringLiteral("#38e86a");
const QString disconnectedStatusColor = QStringLiteral("#ff4b4b");
}  // namespace

/**
 * @brief                       메인 UI를 구성하고 주입된 외부 연동 구현을 연결합니다.
 * @param streamReceiverFactory 영상 수신기 생성 factory
 * @param deviceStatusGatewayFactory 장비 상태 gateway 생성 factory
 * @param dashboardPanelFactory 대시보드 패널 생성 factory
 * @param parent                부모 위젯
 */
MainWindow::MainWindow(std::shared_ptr<StreamReceiverFactory> streamReceiverFactory,
                       std::shared_ptr<DeviceStatusGatewayFactory> deviceStatusGatewayFactory,
                       std::shared_ptr<DashboardPanelFactory> dashboardPanelFactory,
                       std::shared_ptr<ReportGateway> reportGateway, VideoRuntimeConfig videoConfig, QWidget* parent)
    : QMainWindow(parent),
      ui_(std::make_shared<Ui::MainWindow>()),
      deviceStatusGatewayFactory_(std::move(deviceStatusGatewayFactory)),
      dashboardPanelFactory_(std::move(dashboardPanelFactory)),
      reportGateway_(std::move(reportGateway)),
      videoConfig_(std::move(videoConfig)) {
    ui_->setupUi(this);

    streamConfigs_ = videoConfig_.streams;
    setupDashboardLayout();
    setupTopBarStatuses();
    setupClock();
    setupDashboardPanels();
    setupDashboardPanelCoordinator();
    setupDeviceStatusService();
    setupVideoViewEvents();
    setupReportActions();
    setupStreamSessionManager(std::move(streamReceiverFactory));
}

/**
 * @brief 채널별 신고 버튼과 재사용 가능한 확인 다이얼로그를 연결합니다.
 */
void MainWindow::setupReportActions() {
    reportConfirmationDialog_ = new ReportConfirmationDialog(this);
    reportSuccessDialog_ = new ReportSuccessDialog(this);

    connect(reportConfirmationDialog_, &ReportConfirmationDialog::reportConfirmed, this, &MainWindow::sendReport);

    if (reportGateway_) {
        connect(reportGateway_.get(), &ReportGateway::reportSucceeded, this, [this](int channelNumber, const QString&) {
            reportInProgress_ = false;
            setReportButtonsEnabled(true);
            openReportSuccessDialog(channelNumber);
        });
        connect(reportGateway_.get(), &ReportGateway::reportFailed, this, &MainWindow::handleReportFailure);
    }

    const std::array<QPushButton*, requiredCctvChannelCount> reportButtons = {
        ui_->reportChannelButton1, ui_->reportChannelButton2, ui_->reportChannelButton3, ui_->reportChannelButton4};

    for (int channelIndex = 0; channelIndex < static_cast<int>(reportButtons.size()); ++channelIndex) {
        connect(reportButtons[static_cast<std::size_t>(channelIndex)], &QPushButton::clicked, this,
                [this, channelIndex]() { openReportConfirmationDialog(channelIndex + 1); });
    }
}

/**
 * @brief               선택한 채널의 현재 상태를 포함한 Slack 신고를 비동기로 요청합니다.
 * @param channelNumber 사용자에게 표시되는 1부터 4까지의 채널 번호
 */
void MainWindow::sendReport(int channelNumber) {
    if (reportInProgress_) {
        return;
    }

    if (!reportGateway_) {
        handleReportFailure(channelNumber, QStringLiteral("신고 전송기가 구성되지 않았습니다."));
        return;
    }

    reportInProgress_ = true;
    setReportButtonsEnabled(false);

    ReportRequest request;
    request.reportId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.riskLevel = reportRiskLevel(channelNumber);
    request.detail = QStringLiteral("CCTV 관제 사용자가 안전 센터 신고를 요청했습니다.");
    request.reportedAt = QDateTime::currentDateTime();
    request.channelNumber = channelNumber;
    reportGateway_->sendReport(std::move(request));
}

/**
 * @brief               Slack 신고 실패를 안내하고 신고 버튼을 다시 활성화합니다.
 * @param channelNumber 실패한 신고의 채널 번호
 * @param error         Slack 또는 네트워크 오류 설명
 */
void MainWindow::handleReportFailure(int channelNumber, const QString& error) {
    reportInProgress_ = false;
    setReportButtonsEnabled(true);
    qWarning().noquote() << QStringLiteral("[REPORT ERROR] channel=%1 error=%2").arg(channelNumber).arg(error);
    QMessageBox::warning(
        this, QStringLiteral("신고 실패"),
        QStringLiteral("CH %1 신고를 전송하지 못했습니다.\n%2").arg(channelNumber, 2, 10, QLatin1Char('0')).arg(error));
}

/**
 * @brief         네 채널 신고 버튼의 활성 상태를 함께 변경합니다.
 * @param enabled 버튼 활성 여부
 */
void MainWindow::setReportButtonsEnabled(bool enabled) {
    const std::array<QPushButton*, requiredCctvChannelCount> reportButtons = {
        ui_->reportChannelButton1, ui_->reportChannelButton2, ui_->reportChannelButton3, ui_->reportChannelButton4};
    for (QPushButton* button : reportButtons) {
        if (button) {
            button->setEnabled(enabled);
        }
    }
}

/**
 * @brief               신고 시점의 채널 위험 단계를 사용자 표시 문자열로 변환합니다.
 * @param channelNumber 사용자에게 표시되는 1부터 4까지의 채널 번호
 * @return              정상, 주의 또는 위험
 */
QString MainWindow::reportRiskLevel(int channelNumber) const {
    const qsizetype channelIndex = channelNumber - 1;
    if (channelIndex < 0 || channelIndex >= latestVideoRiskLevels_.size()) {
        return QStringLiteral("정상");
    }

    if (latestVideoRiskLevels_[channelIndex] == DigitalTwinRiskLevel::Danger) {
        return QStringLiteral("위험");
    }
    if (latestVideoRiskLevels_[channelIndex] == DigitalTwinRiskLevel::Warning) {
        return QStringLiteral("주의");
    }
    return QStringLiteral("정상");
}

/**
 * @brief               선택한 채널의 신고 여부를 묻는 모달 다이얼로그를 엽니다.
 * @param channelNumber 사용자에게 표시할 1부터 4까지의 채널 번호
 */
void MainWindow::openReportConfirmationDialog(int channelNumber) {
    if (!reportConfirmationDialog_) {
        return;
    }

    reportConfirmationDialog_->setChannelNumber(channelNumber);
    reportConfirmationDialog_->open();
    reportConfirmationDialog_->raise();
    reportConfirmationDialog_->activateWindow();
}

/**
 * @brief               선택한 채널의 안전 센터 신고 완료 안내를 표시합니다.
 * @param channelNumber 사용자에게 표시할 1부터 4까지의 채널 번호
 */
void MainWindow::openReportSuccessDialog(int channelNumber) {
    if (!reportSuccessDialog_) {
        return;
    }

    reportSuccessDialog_->setChannelNumber(channelNumber);
    reportSuccessDialog_->open();
    reportSuccessDialog_->raise();
    reportSuccessDialog_->activateWindow();
}

/**
 * @brief   대시보드 레이아웃 정책을 적용하고 카메라 선택 라벨을 초기화합니다.
 */
void MainWindow::setupDashboardLayout() {
    DashboardLayout::apply(this, ui_.get());
    ui_->titleLabel->setTextFormat(Qt::RichText);
    ui_->titleLabel->setText(
        QStringLiteral("<span style=\"color:#a8d4ff;\">Wise AI</span>"
                       "<span style=\"color:#ffffff;\"> 기반 주차장 디지털 트윈 관제 시스템</span>"));

    const QPixmap settingsIcon(QStringLiteral(":/icons/config_icon.png"));
    ui_->settingsLabel->setText({});
    ui_->settingsLabel->setPixmap(settingsIcon.scaled(QSize(29, 29), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    ui_->settingsLabel->setAlignment(Qt::AlignCenter);
    ui_->settingsLabel->setFixedSize(65, 49);
    ui_->settingsLabel->setFocusPolicy(Qt::StrongFocus);
    ui_->settingsLabel->installEventFilter(this);
    ui_->settingsLabel->setToolTip(QStringLiteral("설정"));

    const QPixmap reportIcon(QStringLiteral(":/icons/report_icon.png"));
    if (!reportIcon.isNull()) {
        ui_->reportActionIconLabel->setPixmap(
            reportIcon.scaled(QSize(23, 23), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        qWarning() << "[MainWindow] Failed to load report icon resource";
    }
    ui_->reportActionIconLabel->setContentsMargins(0, 3, 0, 0);
    ui_->reportActionIconLabel->setAlignment(Qt::AlignCenter);

    mapSettingsDialog_ = new MapSettingsDialog(this);
    connect(mapSettingsDialog_, &MapSettingsDialog::settingsApplied, this,
            [this](const DigitalTwinMapDisplaySettings& settings, bool videoRiskBordersEnabled, bool faceBlurEnabled,
                   bool licensePlateBlurEnabled) {
                mapDisplaySettings_ = settings;
                videoRiskBordersEnabled_ = videoRiskBordersEnabled;
                faceBlurEnabled_ = faceBlurEnabled;
                licensePlateBlurEnabled_ = licensePlateBlurEnabled;
                if (ui_->digitalTwinMapWidget) {
                    ui_->digitalTwinMapWidget->applyDisplaySettings(mapDisplaySettings_);
                }
                if (streamSessionManager_) {
                    streamSessionManager_->setBlurTargetsEnabled(faceBlurEnabled_, licensePlateBlurEnabled_);
                }
                updateVideoRiskBorders(latestVideoRiskLevels_);
            });
}

/**
 * @brief         우측 상단 설정 버튼의 마우스와 키보드 입력을 팝업 열기로 변환합니다.
 * @param watched 이벤트를 받은 객체
 * @param event   전달된 Qt 이벤트
 * @return        설정 열기 입력을 처리했으면 true
 */
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == ui_->settingsLabel && event) {
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                openMapSettingsDialog();
                return true;
            }
        }

        if (event->type() == QEvent::KeyPress) {
            const auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter ||
                keyEvent->key() == Qt::Key_Space) {
                openMapSettingsDialog();
                return true;
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

/**
 * @brief 설정 팝업을 열고 적용된 경우에만 디지털 트윈 맵 표시 상태를 변경합니다.
 */
void MainWindow::openMapSettingsDialog() {
    if (!mapSettingsDialog_) {
        return;
    }

    mapSettingsDialog_->setSettings(mapDisplaySettings_);
    mapSettingsDialog_->setVideoRiskBordersEnabled(videoRiskBordersEnabled_);
    mapSettingsDialog_->setBlurTargetsEnabled(faceBlurEnabled_, licensePlateBlurEnabled_);
    mapSettingsDialog_->setGeometry(rect());
    mapSettingsDialog_->show();
    mapSettingsDialog_->raise();
}

/**
 * @brief 상단 시스템 및 CCTV 연결 상태를 연결 대기 상태로 초기화합니다.
 */
void MainWindow::setupTopBarStatuses() {
    updateSystemStatus(false);
    streamChannelReady_.fill(false, requiredCctvChannelCount);
    updateStreamConnectionStatus();
}

/**
 * @brief 상단 시계를 실제 시스템 시각과 1초 주기로 동기화합니다.
 */
void MainWindow::setupClock() {
    clockTimer_.setInterval(1000);
    clockTimer_.setTimerType(Qt::CoarseTimer);
    connect(&clockTimer_, &QTimer::timeout, this, &MainWindow::updateCurrentDateTime);
    updateCurrentDateTime();
    clockTimer_.start();
}

/**
 * @brief 현재 로컬 날짜와 시각을 상단 표시줄에 반영합니다.
 */
void MainWindow::updateCurrentDateTime() {
    ui_->dateLabel->setText(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

/**
 * @brief            MQTT 프로토콜 연결 여부를 상단 시스템 상태에 반영합니다.
 * @param connected  MQTT broker와 정상적으로 연결되었다면 true
 */
void MainWindow::updateSystemStatus(bool connected) {
    setTopBarStatus(ui_->systemStatusLabel, QStringLiteral("통신 상태"),
                    connected ? QStringLiteral("● 정상") : QStringLiteral("● 연결 중"),
                    connected ? normalStatusColor : disconnectedStatusColor);
}

/**
 * @brief   네 CCTV 채널이 모두 첫 프레임을 수신했는지 상단 연결 상태에 반영합니다.
 */
void MainWindow::updateStreamConnectionStatus() {
    const bool allStreamsReady =
        streamChannelReady_.size() == requiredCctvChannelCount &&
        std::all_of(streamChannelReady_.cbegin(), streamChannelReady_.cend(), [](bool ready) { return ready; });

    setTopBarStatus(ui_->connectionStatusLabel, QStringLiteral("CCTV 상태"),
                    allStreamsReady ? QStringLiteral("● 정상") : QStringLiteral("● 연결 중"),
                    allStreamsReady ? normalStatusColor : disconnectedStatusColor);
}

/**
 * @brief         상단 상태 QLabel에서 제목과 상태 문구를 서로 다른 색으로 표시합니다.
 * @param label   갱신할 상단 상태 QLabel
 * @param title   항상 기본 글자색으로 표시할 상태 제목
 * @param status  상태 점을 포함한 상태 문구
 * @param color   상태 문구에 적용할 RGB 색상 문자열
 */
void MainWindow::setTopBarStatus(QLabel* label, const QString& title, const QString& status, const QString& color) {
    if (!label) {
        return;
    }

    label->setTextFormat(Qt::RichText);
    label->setText(QStringLiteral("<span style=\"color:#d5dfec;font-weight:800;\">%1</span>"
                                  "&nbsp;&nbsp;<span style=\"color:%2;font-weight:800;\">%3</span>")
                       .arg(title, color, status));
}

/**
 * @brief   주입된 factory를 통해 대시보드 패널을 생성하고 placeholder에 배치합니다.
 */
void MainWindow::setupDashboardPanels() {
    if (!dashboardPanelFactory_) {
        qWarning() << "[MainWindow] Dashboard panel factory is not configured";
        return;
    }

    ui_->deviceStatusTitleLabel->setVisible(false);

    DashboardPanelHosts hosts;
    hosts.deviceStatusHost = ui_->deviceStatusBodyFrame;
    hosts.eventLogHost = ui_->eventLogBodyFrame;
    hosts.objectListHost = ui_->objectListBodyFrame;

    const DashboardPanels panels = dashboardPanelFactory_->createPanels(hosts);
    deviceStatusPanel_ = panels.deviceStatusPanel;
    eventLogPanel_ = panels.eventLogPanel;
    objectListPanel_ = panels.objectListPanel;
}

/**
 * @brief   패널 데이터 갱신 경로를 MainWindow 밖의 coordinator에 연결합니다.
 */
void MainWindow::setupDashboardPanelCoordinator() {
    if (dashboardPanelCoordinator_) {
        return;
    }

    dashboardPanelCoordinator_ =
        new DashboardPanelCoordinator(deviceStatusPanel_, eventLogPanel_, objectListPanel_, this);

    if (!ui_->digitalTwinMapWidget) {
        return;
    }

    connect(ui_->digitalTwinMapWidget, &DigitalTwinMapWidget::simulationSnapshotUpdated, dashboardPanelCoordinator_,
            &DashboardPanelCoordinator::consumeDigitalTwinSnapshot, Qt::QueuedConnection);
    connect(ui_->digitalTwinMapWidget, &DigitalTwinMapWidget::liveRiskStreamActivated, dashboardPanelCoordinator_,
            &DashboardPanelCoordinator::resetEventLogForLiveInput, Qt::QueuedConnection);
    connect(ui_->digitalTwinMapWidget, &DigitalTwinMapWidget::channelRiskLevelsChanged, this,
            &MainWindow::updateVideoRiskBorders);
}

/**
 * @brief   장비 상태 service를 생성해 coordinator에 연결하고 worker를 시작합니다.
 */
void MainWindow::setupDeviceStatusService() {
    if (deviceStatusService_) {
        return;
    }

    if (!deviceStatusGatewayFactory_) {
        qWarning() << "[MainWindow] Device status gateway factory is not configured";
        return;
    }

    deviceStatusService_ = std::make_shared<DeviceStatusService>(deviceStatusGatewayFactory_);

    connect(deviceStatusService_.get(), &DeviceStatusService::brokerConnectionChanged, this,
            &MainWindow::updateSystemStatus, Qt::QueuedConnection);

    if (ui_->digitalTwinMapWidget) {
        connect(deviceStatusService_.get(), &DeviceStatusService::brokerConnectionChanged, ui_->digitalTwinMapWidget,
                &DigitalTwinMapWidget::setDeviceSignalAvailable, Qt::QueuedConnection);
        connect(deviceStatusService_.get(), &DeviceStatusService::channelStatusesReceived, ui_->digitalTwinMapWidget,
                &DigitalTwinMapWidget::applyDeviceChannelStatuses, Qt::QueuedConnection);
        connect(deviceStatusService_.get(), &DeviceStatusService::riskFrameReceived, ui_->digitalTwinMapWidget,
                &DigitalTwinMapWidget::applyRiskFrame);
        connect(deviceStatusService_.get(), &DeviceStatusService::centralEventReceived, ui_->digitalTwinMapWidget,
                &DigitalTwinMapWidget::applyCentralEvent, Qt::QueuedConnection);
    }

    if (dashboardPanelCoordinator_) {
        dashboardPanelCoordinator_->bindDeviceStatusService(deviceStatusService_.get());
        connect(deviceStatusService_.get(), &DeviceStatusService::centralEventReceived, dashboardPanelCoordinator_,
                &DashboardPanelCoordinator::consumeCentralEvent, Qt::QueuedConnection);
    }

    deviceStatusService_->start();
}

/**
 * @brief 외부 연동 service와 영상 스트림 세션을 종료합니다.
 */
MainWindow::~MainWindow() {
    if (streamSessionManager_) {
        streamSessionManager_->stop();
    }

    if (deviceStatusService_) {
        deviceStatusService_->stop();
    }
}

/**
 * @brief       창 크기 변화에 맞춰 하단 패널 높이와 목록 컬럼 폭을 보정합니다.
 * @param event  Qt resize 이벤트
 */
void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    updateDashboardAdaptiveSizes();

    if (mapSettingsDialog_ && mapSettingsDialog_->isVisible()) {
        mapSettingsDialog_->setGeometry(rect());
    }
}

/**
 * @brief       창이 처음 표시된 뒤 RTSP 스트림 세션을 시작합니다.
 * @param event Qt show 이벤트
 */
void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    updateDashboardAdaptiveSizes();

    if (streamSessionStarted_ || !streamSessionManager_) {
        return;
    }

    streamSessionStarted_ = true;

    QTimer::singleShot(videoConfig_.initialStartDelayMsec, this, [this]() {
        if (streamSessionManager_) {
            streamSessionManager_->start();
        }
    });
}

/**
 * @brief   현재 창 크기에 맞춰 대시보드 하단 영역과 객체 목록 폭을 보정합니다.
 */
void MainWindow::updateDashboardAdaptiveSizes() { DashboardLayout::adjustBottomSectionHeight(this, ui_.get()); }

/**
 * @brief 4분할 영상 위젯의 더블클릭 이벤트를 확대/복구 동작에 연결합니다.
 */
void MainWindow::setupVideoViewEvents() {
    videoWidgets_ = {
        ui_->camView1,
        ui_->camView2,
        ui_->camView3,
        ui_->camView4,
    };

    auto* grid = ui_->videoGridLayout;

    if (!grid) {
        qWarning() << "[MainWindow] videoGridLayout is null";
        return;
    }

    videoTileFrames_.reserve(videoWidgets_.size());

    for (qsizetype index = 0; index < videoWidgets_.size(); ++index) {
        auto* widget = videoWidgets_[index];

        if (!widget) {
            continue;
        }

        auto* clickable = qobject_cast<ClickableVideoWidget*>(widget);

        if (!clickable) {
            qDebug() << "[MainWindow] Not ClickableVideoWidget:" << widget->objectName();
            continue;
        }

        clickable->setChannelName(QStringLiteral("CH %1").arg(index + 1, 2, 10, QLatin1Char('0')));
        clickable->setExpandedView(false);

        grid->removeWidget(widget);

        auto* tileFrame = new VideoRiskBorderFrame(ui_->cctvCard);
        tileFrame->setObjectName(QStringLiteral("videoTileFrame"));
        tileFrame->setProperty("hovered", false);
        tileFrame->setProperty("riskLevel", QStringLiteral("normal"));
        tileFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        auto* tileLayout = new QVBoxLayout(tileFrame);
        tileLayout->setContentsMargins(2, 2, 2, 2);
        tileLayout->setSpacing(0);
        tileLayout->addWidget(widget);

        grid->addWidget(tileFrame, static_cast<int>(index / 2), static_cast<int>(index % 2));
        videoTileFrames_.append(tileFrame);

        connect(clickable, &ClickableVideoWidget::doubleClicked, this,
                [this](ClickableVideoWidget* target) { toggleExpandVideo(target); });
        connect(clickable, &ClickableVideoWidget::hoverChanged, tileFrame, [tileFrame](bool hovered) {
            const bool riskActive = tileFrame->property("riskLevel").toString() != QStringLiteral("normal");
            tileFrame->setProperty("hovered", hovered && !riskActive);
            tileFrame->style()->unpolish(tileFrame);
            tileFrame->style()->polish(tileFrame);
            tileFrame->update();
        });
    }
}

/**
 * @brief             디지털 트윈의 채널별 위험 상태를 CCTV 타일 테두리에 반영합니다.
 * @param riskLevels  CH-01부터 CH-04까지의 현재 위험 단계
 */
void MainWindow::updateVideoRiskBorders(const QVector<DigitalTwinRiskLevel>& riskLevels) {
    latestVideoRiskLevels_ = riskLevels;
    const qsizetype count = qMin(videoTileFrames_.size(), riskLevels.size());

    for (qsizetype index = 0; index < count; ++index) {
        VideoRiskBorderFrame* tileFrame = videoTileFrames_[index];
        if (!tileFrame) {
            continue;
        }

        const DigitalTwinRiskLevel visibleRiskLevel =
            videoRiskBordersEnabled_ ? riskLevels[index] : DigitalTwinRiskLevel::Normal;

        QString riskName = QStringLiteral("normal");
        if (visibleRiskLevel == DigitalTwinRiskLevel::Danger) {
            riskName = QStringLiteral("danger");
        } else if (visibleRiskLevel == DigitalTwinRiskLevel::Warning) {
            riskName = QStringLiteral("warning");
        }

        if (tileFrame->property("riskLevel").toString() == riskName) {
            continue;
        }

        tileFrame->setProperty("riskLevel", riskName);
        tileFrame->setRiskLevel(visibleRiskLevel);
        const bool hovered =
            riskName == QStringLiteral("normal") && videoWidgets_[index] && videoWidgets_[index]->underMouse();
        tileFrame->setProperty("hovered", hovered);
        tileFrame->style()->unpolish(tileFrame);
        tileFrame->style()->polish(tileFrame);
        tileFrame->update();
    }
}

/**
 * @brief                  영상 출력 창과 스트림 설정을 세션 관리자에 연결합니다.
 * @param receiverFactory  채널별 영상 수신기 생성 factory
 */
void MainWindow::setupStreamSessionManager(std::shared_ptr<StreamReceiverFactory> receiverFactory) {
    if (streamSessionManager_) {
        return;
    }

    if (!receiverFactory) {
        qWarning() << "[MainWindow] Stream receiver factory is not configured";
        return;
    }

    streamSessionManager_ =
        new StreamSessionManager(std::move(receiverFactory), videoConfig_.receiverStartSpacingMsec, this);
    streamSessionManager_->setBlurTargetsEnabled(faceBlurEnabled_, licensePlateBlurEnabled_);

    if (deviceStatusService_) {
        connect(deviceStatusService_.get(), &DeviceStatusService::blurFrameReceived, streamSessionManager_,
                &StreamSessionManager::submitBlurFrame, Qt::AutoConnection);
    }

    connect(streamSessionManager_, &StreamSessionManager::loadingChanged, this, [this](int channelIndex, bool loading) {
        if (channelIndex < 0 || channelIndex >= videoWidgets_.size()) {
            qWarning() << "[MainWindow] Invalid loading channel index:" << channelIndex;
            return;
        }

        auto* videoWidget = qobject_cast<ClickableVideoWidget*>(videoWidgets_[channelIndex]);

        if (videoWidget) {
            videoWidget->setLoading(loading);
        }

        if (loading && channelIndex < streamChannelReady_.size()) {
            streamChannelReady_[channelIndex] = false;
            updateStreamConnectionStatus();
        }
    });

    connect(streamSessionManager_, &StreamSessionManager::errorOccurred, this,
            [this](int channelIndex, const QString& error) {
                qWarning().noquote() << QStringLiteral("[Channel %1] %2").arg(channelIndex + 1).arg(error);

                if (channelIndex >= 0 && channelIndex < streamChannelReady_.size()) {
                    streamChannelReady_[channelIndex] = false;
                    updateStreamConnectionStatus();
                }
            });

    connect(streamSessionManager_, &StreamSessionManager::firstFrameReceived, this, [this](int channelIndex) {
        if (channelIndex < 0 || channelIndex >= streamChannelReady_.size()) {
            return;
        }

        streamChannelReady_[channelIndex] = true;
        updateStreamConnectionStatus();

        auto* videoWidget = qobject_cast<ClickableVideoWidget*>(videoWidgets_.value(channelIndex));
        if (!videoWidget) {
            return;
        }

        videoWidget->refreshChannelLabel();
        QTimer::singleShot(150, videoWidget, &ClickableVideoWidget::refreshChannelLabel);
        QTimer::singleShot(600, videoWidget, &ClickableVideoWidget::refreshChannelLabel);
    });

    QVector<StreamOutputBinding> bindings;

    const qsizetype streamCount = qMin(videoWidgets_.size(), streamConfigs_.size());

    bindings.reserve(streamCount);

    for (qsizetype index = 0; index < streamCount; ++index) {
        QWidget* outputWidget = videoWidgets_[index];
        const StreamConfig& config = streamConfigs_[index];

        if (!outputWidget || !config.enabled) {
            continue;
        }

        outputWidget->setAttribute(Qt::WA_NativeWindow);
        outputWidget->setAttribute(Qt::WA_DontCreateNativeAncestors);

        if (auto* videoWidget = qobject_cast<ClickableVideoWidget*>(outputWidget)) {
            videoWidget->setLoading(true);
        }

        StreamOutputBinding binding;
        binding.config = config;
        binding.outputWindowHandle = outputWidget->winId();

        bindings.append(std::move(binding));
    }

    streamSessionManager_->configure(std::move(bindings));
}

/**
 * @brief              현재 확대 상태에 따라 대상 영상을 확대하거나 4분할로 복구합니다.
 * @param targetWidget  더블클릭된 영상 위젯
 */
void MainWindow::toggleExpandVideo(QWidget* targetWidget) {
    if (!targetWidget) {
        return;
    }

    if (expandedWidget_ == nullptr) {
        expandVideo(targetWidget);
    } else {
        restoreVideoGrid();
    }
}

/**
 * @brief              선택한 영상 위젯을 CCTV 영역 전체로 확장합니다.
 * @param targetWidget  확대할 영상 위젯
 */
void MainWindow::expandVideo(QWidget* targetWidget) {
    auto* grid = ui_->videoGridLayout;

    if (!grid) {
        qDebug() << "[MainWindow] videoGridLayout is null";
        return;
    }

    const qsizetype targetIndex = videoWidgets_.indexOf(targetWidget);

    if (targetIndex < 0 || targetIndex >= videoTileFrames_.size()) {
        qWarning() << "[MainWindow] Video tile frame is not configured";
        return;
    }

    QFrame* targetFrame = videoTileFrames_[targetIndex];

    for (auto* widget : videoWidgets_) {
        if (auto* videoWidget = qobject_cast<ClickableVideoWidget*>(widget)) {
            videoWidget->setExpandedView(widget == targetWidget);
        }
    }

    for (auto* frame : videoTileFrames_) {
        if (frame && frame != targetFrame) {
            frame->hide();
        }
    }

    grid->removeWidget(targetFrame);
    grid->addWidget(targetFrame, 0, 0, 2, 2);

    targetFrame->show();
    targetFrame->raise();

    expandedWidget_ = targetWidget;
}

/**
 * @brief   확대된 영상을 원래 2x2 영상 그리드로 복구합니다.
 */
void MainWindow::restoreVideoGrid() {
    auto* grid = ui_->videoGridLayout;

    if (!grid) {
        qDebug() << "[MainWindow] videoGridLayout is null";
        return;
    }

    for (auto* frame : videoTileFrames_) {
        if (frame) {
            grid->removeWidget(frame);
        }
    }

    if (videoTileFrames_.size() == 4) {
        grid->addWidget(videoTileFrames_[0], 0, 0);
        grid->addWidget(videoTileFrames_[1], 0, 1);
        grid->addWidget(videoTileFrames_[2], 1, 0);
        grid->addWidget(videoTileFrames_[3], 1, 1);
    }

    for (auto* frame : videoTileFrames_) {
        if (frame) {
            frame->show();
        }
    }

    for (auto* widget : videoWidgets_) {
        if (auto* videoWidget = qobject_cast<ClickableVideoWidget*>(widget)) {
            videoWidget->setExpandedView(false);
        }
    }

    expandedWidget_ = nullptr;
}
