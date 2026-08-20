#include "ui/mainwindow.h"

#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QKeySequence>
#include <QMessageBox>
#include <QQuickItem>
#include <QQuickWidget>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <algorithm>
#include <array>
#include <memory>
#include <utility>

#include "model/DigitalTwinTypes.h"
#include "network/gateways/DeviceStatusGatewayFactory.h"
#include "network/gateways/ReportGateway.h"
#include "network/services/DeviceStatusService.h"
#include "ui/ClickableVideoWidget.h"
#include "ui/DashboardLayout.h"
#include "ui/DigitalTwinMapWidget.h"
#include "ui/SharedQmlEngine.h"
#include "ui/dialogs/MapSettingsDialog.h"
#include "ui/panels/DashboardPanelCoordinator.h"
#include "ui/panels/DashboardPanelFactory.h"
#include "ui/panels/DeviceStatusPanel.h"
#include "ui/panels/EventLogPanel.h"
#include "ui/panels/ObjectListPanel.h"
#include "ui_mainwindow.h"
#include "video/StreamReceiverFactory.h"
#include "video/StreamSessionManager.h"

namespace {
const QString normalStatusColor = QStringLiteral("#38e86a");
const QString disconnectedStatusColor = QStringLiteral("#ff4b4b");
}  // namespace

/**
 * @brief                       메인 UI를 구성하고 주입된 외부 연동 구현을
 * 연결합니다.
 * @param streamReceiverFactory 영상 수신기 생성 factory
 * @param deviceStatusGatewayFactory 장비 상태 gateway 생성 factory
 * @param dashboardPanelFactory 대시보드 패널 생성 factory
 * @param parent                부모 위젯
 */
MainWindow::MainWindow(std::shared_ptr<StreamReceiverFactory> streamReceiverFactory,
                       std::shared_ptr<DeviceStatusGatewayFactory> deviceStatusGatewayFactory,
                       std::shared_ptr<DashboardPanelFactory> dashboardPanelFactory,
                       std::shared_ptr<ReportGateway> reportGateway, VideoRuntimeConfig videoConfig,
                       DigitalTwinRuntimeConfig digitalTwinConfig, QString configSourcePath, QWidget* parent)
    : QMainWindow(parent),
      ui_(std::make_shared<Ui::MainWindow>()),
      deviceStatusGatewayFactory_(std::move(deviceStatusGatewayFactory)),
      dashboardPanelFactory_(std::move(dashboardPanelFactory)),
      reportGateway_(std::move(reportGateway)),
      videoConfig_(std::move(videoConfig)),
      configSourcePath_(std::move(configSourcePath)) {
    ui_->setupUi(this);
    if (ui_->digitalTwinMapWidget) {
        ui_->digitalTwinMapWidget->configureLiveTracking(digitalTwinConfig);
    }

    streamConfigs_ = videoConfig_.streams;
    currentVideoAreaIndex_ = videoConfig_.areas.isEmpty() ? 0
                                                          : qBound(0, videoConfig_.initialAreaIndex,
                                                                   static_cast<int>(videoConfig_.areas.size()) - 1);
    videoPreprocessingSettingsByChannel_.fill(videoConfig_.receiver.preprocessing, streamConfigs_.size());
    latestVideoRiskLevels_.fill(DigitalTwinRiskLevel::Normal, streamConfigs_.size());
    setupDashboardLayout();
    setupQuickTopBar();
    setupQuickDashboardChrome();
    setupQuickPanelHeaders();
    setupQuickDialogOverlay();
    setupQuickGuideDialog();
    setupTopBarStatuses();
    setupClock();
    setupWindowShortcuts();
    setupDashboardPanels();
    setupDashboardPanelCoordinator();
    setupDeviceStatusService();
    setupVideoViewEvents();
    setupVideoAreaSelector();
    setupReportActions();
    setupStreamSessionManager(std::move(streamReceiverFactory));
}

/** @brief Alt+Enter로 전체 화면을 전환하고 F1으로 사용 안내를 여는 단축키를 등록합니다.
 */
void MainWindow::setupWindowShortcuts() {
    auto* returnShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Return), this);
    auto* enterShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Enter), this);
    connect(returnShortcut, &QShortcut::activated, this, &MainWindow::toggleFullScreen);
    connect(enterShortcut, &QShortcut::activated, this, &MainWindow::toggleFullScreen);

    auto* guideShortcut = new QShortcut(QKeySequence(Qt::Key_F1), this);
    connect(guideShortcut, &QShortcut::activated, this, &MainWindow::openGuideDialog);
}

/** @brief 전체 화면으로 전환하거나 전체 화면 진입 전의 창 상태로 복원합니다. */
void MainWindow::toggleFullScreen() {
    if (isFullScreen()) {
        setWindowState(windowStateBeforeFullScreen_);
        return;
    }

    windowStateBeforeFullScreen_ = windowState();
    windowStateBeforeFullScreen_.setFlag(Qt::WindowFullScreen, false);
    showFullScreen();
}

/**
 * @brief 채널별 신고 버튼과 재사용 가능한 확인 다이얼로그를 연결합니다.
 */
void MainWindow::setupReportActions() {
    if (reportGateway_) {
        connect(reportGateway_.get(), &ReportGateway::reportSucceeded, this, [this](int channelNumber, const QString&) {
            reportInProgress_ = false;
            setReportButtonsEnabled(true);
            openReportSuccessDialog(channelNumber);
        });
        connect(reportGateway_.get(), &ReportGateway::reportFailed, this, &MainWindow::handleReportFailure);
    }
}

/**
 * @brief               선택한 채널의 현재 상태를 포함한 Slack 신고를 비동기로
 * 요청합니다.
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
    request.areaName = videoConfig_.areas.value(currentVideoAreaIndex_).name;
    request.riskLevel = reportRiskLevel(channelNumber);
    request.detail = QStringLiteral("CCTV 관제 사용자가 안전 센터 신고를 요청했습니다.");
    request.reportedAt = QDateTime::currentDateTime();
    request.channelNumber = channelNumber;
    reportGateway_->sendReport(std::move(request));
}

/**
 * @brief               Slack 신고 실패를 안내하고 신고 버튼을 다시
 * 활성화합니다.
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
    if (quickCctvToolbar_ && quickCctvToolbar_->rootObject()) {
        quickCctvToolbar_->rootObject()->setProperty("reportEnabled", enabled);
    }
}

/**
 * @brief           현재 표시 구역의 슬롯을 사용자 표시 채널 번호로 변환합니다.
 * @param slotIndex 0부터 3까지의 화면 슬롯 인덱스
 * @return          사용자 표시용 1 기반 채널 번호
 */
int MainWindow::reportChannelNumberForSlot(int slotIndex) const { return slotIndex + 1; }

/**
 * @brief               신고 시점의 채널 위험 단계를 사용자 표시 문자열로
 * 변환합니다.
 * @param channelNumber 사용자에게 표시되는 1부터 4까지의 채널 번호
 * @return              정상, 주의 또는 위험
 */
QString MainWindow::reportRiskLevel(int channelNumber) const {
    const qsizetype channelIndex = videoGlobalChannelIndex(currentVideoAreaIndex_, channelNumber - 1);
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
    pendingReportChannelNumber_ = qBound(1, channelNumber, videoChannelsPerArea);
    quickDialogMode_ = QuickDialogMode::ReportConfirmation;
    setReportDialogChannelProperties(pendingReportChannelNumber_);
    showQuickDialog(QStringLiteral("confirmation"), QStringLiteral("신고 확인"),
                    QStringLiteral("이 채널의 현재 상황을 안전 센터로 신고합니다. 계속하시겠습니까?"));
}

/**
 * @brief               신고 다이얼로그에 대상 채널과 현재 위험 단계를 전달합니다.
 * @param channelNumber 사용자에게 표시할 1부터 4까지의 채널 번호
 */
void MainWindow::setReportDialogChannelProperties(int channelNumber) {
    setQuickDialogProperty("channelText", QStringLiteral("CH %1").arg(channelNumber, 2, 10, QLatin1Char('0')));
    setQuickDialogProperty("riskText", reportRiskLevel(channelNumber));
}

/**
 * @brief               선택한 채널의 안전 센터 신고 완료 안내를 표시합니다.
 * @param channelNumber 사용자에게 표시할 1부터 4까지의 채널 번호
 */
void MainWindow::openReportSuccessDialog(int channelNumber) {
    pendingReportChannelNumber_ = qBound(1, channelNumber, videoChannelsPerArea);
    quickDialogMode_ = QuickDialogMode::ReportSuccess;
    setReportDialogChannelProperties(pendingReportChannelNumber_);
    showQuickDialog(QStringLiteral("success"), QStringLiteral("신고 완료"),
                    QStringLiteral("안전 센터로 신고가 접수되었습니다."));
}

/**
 * @brief   대시보드 레이아웃 정책을 적용하고 카메라 선택 라벨을 초기화합니다.
 */
void MainWindow::setupDashboardLayout() {
    DashboardLayout::apply(this, ui_.get());

    mapSettingsDialog_ = new MapSettingsDialog(this);
    mapSettingsDialog_->setConfigSourcePath(configSourcePath_);
    mapSettingsDialog_->installEventFilter(this);
    connect(mapSettingsDialog_, &MapSettingsDialog::settingsApplied, this,
            [this](const DigitalTwinMapDisplaySettings& settings, bool videoRiskBordersEnabled, bool faceBlurEnabled,
                   bool licensePlateBlurEnabled, int channelIndex,
                   const VideoPreprocessingSettings& preprocessingSettings) {
                mapDisplaySettings_ = settings;
                videoRiskBordersEnabled_ = videoRiskBordersEnabled;
                faceBlurEnabled_ = faceBlurEnabled;
                licensePlateBlurEnabled_ = licensePlateBlurEnabled;
                selectedPreprocessingChannelIndex_ = channelIndex;
                if (channelIndex >= 0 && channelIndex < videoPreprocessingSettingsByChannel_.size()) {
                    videoPreprocessingSettingsByChannel_[channelIndex] = preprocessingSettings;
                }
                if (ui_->digitalTwinMapWidget) {
                    ui_->digitalTwinMapWidget->applyDisplaySettings(mapDisplaySettings_);
                }
                if (streamSessionManager_) {
                    streamSessionManager_->setBlurTargetsEnabled(faceBlurEnabled_, licensePlateBlurEnabled_);
                    streamSessionManager_->setVideoPreprocessingSettings(channelIndex, preprocessingSettings);
                }
                updateVideoRiskBorders(latestVideoRiskLevels_);
            });
    connect(mapSettingsDialog_, &MapSettingsDialog::videoAreaSelected, this, &MainWindow::switchVideoArea);
    connect(mapSettingsDialog_, &MapSettingsDialog::videoPreprocessingApplyRequested, this,
            [this](int channelIndex, const VideoPreprocessingSettings& settings) {
                if (channelIndex < 0) {
                    videoPreprocessingSettingsByChannel_.fill(settings, streamConfigs_.size());
                } else if (channelIndex < videoPreprocessingSettingsByChannel_.size()) {
                    selectedPreprocessingChannelIndex_ = channelIndex;
                    videoPreprocessingSettingsByChannel_[channelIndex] = settings;
                }
                if (streamSessionManager_) {
                    if (channelIndex < 0) {
                        streamSessionManager_->setVideoPreprocessingSettings(settings);
                    } else {
                        streamSessionManager_->setVideoPreprocessingSettings(channelIndex, settings);
                    }
                }
            });
}

/**
 * @brief 기존 QWidget 화면 안에 Qt Quick 상단 표시줄을 배치합니다.
 *
 * @details 영상 출력과 대시보드 패널은 기존 QWidget 구현을 유지하고 상단 표시줄만
 * Qt Design Studio에서 편집 가능한 QML 컴포넌트로 분리합니다.
 */
void MainWindow::setupQuickTopBar() {
    if (!ui_->topBarFrame || !ui_->topBarLayout) {
        return;
    }

    quickTopBar_ = createQuickView(QStringLiteral("TopBar.qml"), ui_->topBarFrame);
    if (!quickTopBar_) {
        return;
    }

    ui_->topBarLayout->setContentsMargins(0, 0, 0, 0);
    ui_->topBarLayout->setSpacing(0);
    ui_->topBarLayout->insertWidget(0, quickTopBar_, 1);

    QObject* rootObject = quickTopBar_->rootObject();
    connect(rootObject, SIGNAL(areaRequested()), this, SLOT(openVideoAreaSelectionDialog()));
    connect(rootObject, SIGNAL(settingsRequested()), this, SLOT(openMapSettingsDialog()));
    connect(rootObject, SIGNAL(guideRequested()), this, SLOT(openGuideDialog()));
}

/** @brief CCTV 조작부와 상태 범례를 공통 Qt Quick 테마로 교체합니다. */
void MainWindow::setupQuickDashboardChrome() {
    if (ui_->cctvHeaderLayout) {
        quickCctvToolbar_ = createQuickView(QStringLiteral("CctvToolbar.qml"), ui_->cctvCard);
        if (quickCctvToolbar_) {
            quickCctvToolbar_->setCursor(Qt::ArrowCursor);
            quickCctvToolbar_->setFixedHeight(qRound(quickCctvToolbar_->rootObject()->implicitHeight()));
            ui_->cctvHeaderLayout->insertWidget(0, quickCctvToolbar_, 1);
            connect(quickCctvToolbar_->rootObject(), SIGNAL(reportRequested(int)), this,
                    SLOT(openReportConfirmationDialog(int)));
        }
    }

    if (ui_->legendLayout) {
        quickLegend_ = createQuickView(QStringLiteral("StatusLegend.qml"), ui_->legendFrame);
        if (quickLegend_) {
            ui_->legendLayout->setContentsMargins(0, 0, 0, 0);
            ui_->legendLayout->insertWidget(0, quickLegend_, 1);
        }
    }
}

/**
 * @brief 맵과 하단 패널의 제목 줄을 공통 Qt Quick 헤더로 올립니다.
 *
 * @details 장비 제어/상태 카드는 제목 없이 채널 카드만 보여주므로 대상에서 제외합니다.
 */
void MainWindow::setupQuickPanelHeaders() {
    addQuickPanelHeader(ui_->mapCardLayout, QStringLiteral("Digital Twin Map"));
    addQuickPanelHeader(ui_->objectListLayout, QStringLiteral("Live Objects"));
    addQuickPanelHeader(ui_->eventLogLayout, QStringLiteral("Event Log"));
}

/**
 * @brief        카드 레이아웃 맨 위에 공통 Qt Quick 제목 줄을 추가합니다.
 * @param layout 제목을 올릴 카드 레이아웃
 * @param title  표시할 제목 문구
 */
void MainWindow::addQuickPanelHeader(QBoxLayout* layout, const QString& title) {
    if (!layout) {
        return;
    }

    QQuickWidget* header = createQuickView(QStringLiteral("PanelHeader.qml"), layout->parentWidget());
    if (!header) {
        return;
    }

    QQuickItem* rootObject = header->rootObject();
    rootObject->setProperty("titleText", title);
    header->setFixedHeight(qRound(rootObject->implicitHeight()));
    layout->insertWidget(0, header);
}

/**
 * @brief 구역 선택과 신고 안내가 공유하는 단일 Qt Quick 오버레이를 준비합니다.
 *
 * @details MapSettingsDialog와 같은 이유로 자식 위젯이 아니라 반투명 최상위 창으로 띄웁니다.
 * 화면을 덮는 자식 위젯이 생기면 나머지 QQuickWidget 합성이 중단됩니다.
 */
void MainWindow::setupQuickDialogOverlay() {
    if (!ui_->centralwidget) {
        return;
    }

    quickDialogOverlay_ =
        createQuickView(QStringLiteral("OverlayDialog.qml"), this, Qt::Dialog | Qt::FramelessWindowHint);
    if (!quickDialogOverlay_) {
        return;
    }

    quickDialogOverlay_->setWindowModality(Qt::WindowModal);
    quickDialogOverlay_->setClearColor(QColor(QStringLiteral("#123a55")));
    quickDialogOverlay_->hide();
    setQuickDialogProperty("selectionAnimated", false);
    connect(quickDialogOverlay_->rootObject(), SIGNAL(accepted(int)), this, SLOT(handleQuickDialogAccepted(int)));
    connect(quickDialogOverlay_->rootObject(), SIGNAL(rejected()), this, SLOT(closeQuickDialog()));
}

/**
 * @brief 상단 표시줄 "?" 버튼과 F1이 여는 사용 안내 창을 준비합니다.
 *
 * @details 오버레이와 같은 이유로 최상위 창으로 띄웁니다. 창을 닫을 때는 hide()만 씁니다 —
 * close()는 마지막 창 닫힘 판정을 타서 앱이 통째로 종료됩니다.
 */
void MainWindow::setupQuickGuideDialog() {
    quickGuideDialog_ = createQuickView(QStringLiteral("GuideDialog.qml"), this, Qt::Dialog | Qt::FramelessWindowHint);
    if (!quickGuideDialog_) {
        return;
    }

    quickGuideDialog_->setWindowModality(Qt::WindowModal);
    quickGuideDialog_->setClearColor(QColor(QStringLiteral("#123a55")));
    quickGuideDialog_->hide();
    connect(quickGuideDialog_->rootObject(), SIGNAL(closed()), this, SLOT(closeGuideDialog()));

    // QQuickWidget은 포커스를 받지 않게 만들어 두어서 QML 쪽 Keys로는 Esc가 오지 않습니다.
    auto* escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), quickGuideDialog_);
    connect(escapeShortcut, &QShortcut::activated, this, &MainWindow::closeGuideDialog);
}

/**
 * @brief 사용 안내 창을 메인 창 가운데에 띄웁니다.
 *
 * @details 설정 팝업이나 오버레이가 떠 있는 동안에는 열지 않습니다. 최상위 QQuickWidget 창 위에
 * 또 다른 최상위 QQuickWidget 창을 겹치면 안쪽이 검은 사각형으로만 그려집니다.
 */
void MainWindow::openGuideDialog() {
    if (!quickGuideDialog_ || !quickGuideDialog_->rootObject()) {
        return;
    }

    if ((mapSettingsDialog_ && mapSettingsDialog_->isVisible()) ||
        (quickDialogOverlay_ && quickDialogOverlay_->isVisible())) {
        return;
    }

    QQuickItem* rootObject = quickGuideDialog_->rootObject();
    rootObject->setProperty("adminMode", sessionIsAdmin_);

    // QML이 알려준 패널 크기를 창 크기로 고정합니다. setGeometry만으로는 위젯 sizeHint에 밀립니다.
    const QSize panelSize(qRound(rootObject->implicitWidth()), qRound(rootObject->implicitHeight()));
    quickGuideDialog_->setFixedSize(panelSize);
    const QWidget* host = ui_->centralwidget;
    const QPoint hostCenter = host->mapToGlobal(host->rect().center());
    quickGuideDialog_->move(hostCenter - QPoint(panelSize.width() / 2, panelSize.height() / 2));
    quickGuideDialog_->show();
    quickGuideDialog_->raise();
    quickGuideDialog_->activateWindow();
}

/** @brief 사용 안내 창을 감춥니다. */
void MainWindow::closeGuideDialog() {
    if (quickGuideDialog_) {
        quickGuideDialog_->hide();
    }
}

/**
 * @brief         공용 QML 엔진 위에 투명 배경 QQuickWidget을 만듭니다.
 * @param qmlFile qrc:/qml 아래의 QML 파일 이름
 * @param parent  QQuickWidget의 부모 위젯
 * @return        루트 객체 로딩까지 성공하면 QQuickWidget, 실패하면 nullptr
 */
QQuickWidget* MainWindow::createQuickView(const QString& qmlFile, QWidget* parent, Qt::WindowFlags windowFlags) {
    auto* view = new QQuickWidget(sharedQmlEngine(), parent);
    // 플래그는 반드시 setSource 이전에 지정합니다. 이후에 바꾸면 네이티브 창이 다시 만들어지면서
    // 이미 올라간 scene graph가 화면에 나오지 않습니다.
    if (windowFlags != Qt::WindowFlags()) {
        view->setWindowFlags(windowFlags);
    }
    view->setObjectName(QStringLiteral("quick") + qmlFile.section(QLatin1Char('.'), 0, 0));
    view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    view->setClearColor(Qt::transparent);
    if (windowFlags == Qt::WindowFlags()) {
        // 자식으로 올릴 때만 필요합니다. 두 속성이 함께 있어야 카드 QSS 배경이 QML 뒤로 비칩니다.
        // 최상위 창에 걸면 이 환경에서는 아무것도 그려지지 않습니다.
        view->setAttribute(Qt::WA_TranslucentBackground);
        view->setAttribute(Qt::WA_AlwaysStackOnTop);
    }
    view->setFocusPolicy(Qt::NoFocus);
    installQuickCursorReset(view);
    view->setSource(QUrl(QStringLiteral("qrc:/qml/") + qmlFile));

    if (!view->rootObject()) {
        qWarning() << "[MainWindow] Failed to load QML view" << qmlFile << view->errors();
        delete view;
        return nullptr;
    }

    return view;
}

/**
 * @brief         우측 상단 설정 버튼의 마우스 클릭을 팝업 열기로 변환합니다.
 * @param watched 이벤트를 받은 객체
 * @param event   전달된 Qt 이벤트
 * @return        설정 열기 입력을 처리했으면 true
 */
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == mapSettingsDialog_ && event && event->type() == QEvent::Hide) {
        unsetCursor();
        if (quickTopBar_) {
            quickTopBar_->setCursor(Qt::ArrowCursor);
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

/**
 * @brief 설정 팝업을 열고 적용된 경우에만 디지털 트윈 맵 표시 상태를
 * 변경합니다.
 */
void MainWindow::openMapSettingsDialog() {
    if (!mapSettingsDialog_) {
        return;
    }

    // 설정 팝업의 구역 관리 탭은 RTSP 계정과 비밀번호를 보여주고 고칠 수 있다.
    // 상단 버튼도 관제사에게는 감추지만, signal은 그 경로 말고도 올 수 있으므로 여기서 한 번 더 막는다
    if (!sessionIsAdmin_) {
        qWarning().noquote() << QStringLiteral("[Session] %1 is not allowed to open settings").arg(sessionUserName_);
        return;
    }

    mapSettingsDialog_->setSettings(mapDisplaySettings_);
    mapSettingsDialog_->setVideoRiskBordersEnabled(videoRiskBordersEnabled_);
    mapSettingsDialog_->setBlurTargetsEnabled(faceBlurEnabled_, licensePlateBlurEnabled_);
    mapSettingsDialog_->setStreamConfigs(streamConfigs_);
    mapSettingsDialog_->setVideoAreas(videoConfig_.areas, currentVideoAreaIndex_);
    mapSettingsDialog_->setVideoPreprocessingSettings(videoPreprocessingSettingsByChannel_,
                                                      selectedPreprocessingChannelIndex_);
    mapSettingsDialog_->setGeometry(rect());
    mapSettingsDialog_->show();
    mapSettingsDialog_->raise();
}

/**
 * @brief 상단 시스템 및 CCTV 연결 상태를 연결 대기 상태로 초기화합니다.
 */
void MainWindow::setupTopBarStatuses() {
    updateSystemStatus(false);
    streamChannelReady_.fill(false, streamConfigs_.size());
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
    const QString dateTime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    setQuickTopBarProperty("dateTimeText", dateTime);
}

/**
 * @brief            MQTT 프로토콜 연결 여부를 상단 시스템 상태에 반영합니다.
 * @param connected  MQTT broker와 정상적으로 연결되었다면 true
 */
void MainWindow::updateSystemStatus(bool connected) {
    setQuickTopBarProperty("systemStatusText", connected ? QStringLiteral("● Online") : QStringLiteral("● Connecting"));
    setQuickTopBarProperty("systemStatusColor", connected ? normalStatusColor : disconnectedStatusColor);
}

/**
 * @brief   네 CCTV 채널이 모두 첫 프레임을 수신했는지 상단 연결 상태에
 * 반영합니다.
 */
void MainWindow::updateStreamConnectionStatus() {
    bool hasEnabledStream = false;
    bool allStreamsReady = true;

    for (int channelIndex : channelsForArea(currentVideoAreaIndex_)) {
        if (channelIndex < 0 || channelIndex >= streamConfigs_.size() || channelIndex >= streamChannelReady_.size()) {
            allStreamsReady = false;
            break;
        }

        if (!streamConfigs_[channelIndex].enabled) {
            continue;
        }

        hasEnabledStream = true;
        if (!streamChannelReady_[channelIndex]) {
            allStreamsReady = false;
            break;
        }
    }

    allStreamsReady = hasEnabledStream && allStreamsReady;

    setQuickTopBarProperty("cctvStatusText",
                           allStreamsReady ? QStringLiteral("● Online") : QStringLiteral("● Connecting"));
    setQuickTopBarProperty("cctvStatusColor", allStreamsReady ? normalStatusColor : disconnectedStatusColor);
}

/** @brief Qt Quick 상단 표시줄의 루트 속성을 안전하게 갱신합니다. */
void MainWindow::setQuickTopBarProperty(const char* name, const QVariant& value) {
    if (quickTopBar_ && quickTopBar_->rootObject()) {
        quickTopBar_->rootObject()->setProperty(name, value);
    }
}

/**
 * @brief   주입된 factory를 통해 대시보드 패널을 생성하고 placeholder에
 * 배치합니다.
 */
void MainWindow::setupDashboardPanels() {
    if (!dashboardPanelFactory_) {
        qWarning() << "[MainWindow] Dashboard panel factory is not configured";
        return;
    }

    DashboardPanelHosts hosts;
    hosts.deviceStatusHost = ui_->deviceStatusBodyFrame;
    hosts.eventLogHost = ui_->eventLogBodyFrame;
    hosts.objectListHost = ui_->objectListBodyFrame;

    const DashboardPanels panels = dashboardPanelFactory_->createPanels(hosts);
    deviceStatusPanel_ = panels.deviceStatusPanel;
    eventLogPanel_ = panels.eventLogPanel;
    objectListPanel_ = panels.objectListPanel;
    if (deviceStatusPanel_) {
        deviceStatusPanel_->setChannelCount(static_cast<int>(streamConfigs_.size()));
    }
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
    // 지도에서 구역을 직접 눌러도 상단 구역 선택과 같은 전환이 일어난다
    connect(ui_->digitalTwinMapWidget, &DigitalTwinMapWidget::zoneSelected, this, &MainWindow::switchVideoArea);
}

/**
 * @brief   장비 상태 service를 생성해 coordinator에 연결하고 worker를
 * 시작합니다.
 */
void MainWindow::setupDeviceStatusService() {
    if (deviceStatusService_) {
        return;
    }

    if (!deviceStatusGatewayFactory_) {
        qWarning() << "[MainWindow] Device status gateway factory is not configured";
        return;
    }

    deviceStatusService_ =
        std::make_shared<DeviceStatusService>(deviceStatusGatewayFactory_, static_cast<int>(streamConfigs_.size()));

    connect(deviceStatusService_.get(), &DeviceStatusService::brokerConnectionChanged, this,
            &MainWindow::updateSystemStatus, Qt::QueuedConnection);

    if (ui_->digitalTwinMapWidget) {
        connect(deviceStatusService_.get(), &DeviceStatusService::brokerConnectionChanged, ui_->digitalTwinMapWidget,
                &DigitalTwinMapWidget::setDeviceSignalAvailable, Qt::QueuedConnection);
        connect(deviceStatusService_.get(), &DeviceStatusService::channelStatusesReceived, ui_->digitalTwinMapWidget,
                &DigitalTwinMapWidget::applyDeviceChannelStatuses, Qt::QueuedConnection);
        connect(deviceStatusService_.get(), &DeviceStatusService::riskFrameReceived, ui_->digitalTwinMapWidget,
                &DigitalTwinMapWidget::applyRiskFrame, Qt::QueuedConnection);
        connect(deviceStatusService_.get(), &DeviceStatusService::centralEventReceived, ui_->digitalTwinMapWidget,
                &DigitalTwinMapWidget::applyCentralEvent, Qt::QueuedConnection);
    }

    if (dashboardPanelCoordinator_) {
        dashboardPanelCoordinator_->bindDeviceStatusService(deviceStatusService_.get());
        connect(deviceStatusService_.get(), &DeviceStatusService::centralEventReceived, dashboardPanelCoordinator_,
                &DashboardPanelCoordinator::consumeCentralEvent, Qt::QueuedConnection);
    }

    // 연결만 걸어 두고 실제 수신은 beginSession()에서 시작한다. 로그인 화면 뒤에서 영상과
    // 장비 상태가 이미 돌고 있으면 이 관문이 아무것도 막지 못한다
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
    if (quickDialogOverlay_ && quickDialogOverlay_->isVisible()) {
        quickDialogOverlay_->setGeometry(quickDialogHostGeometry());
    }
}

/**
 * @brief       창이 처음 표시된 뒤 RTSP 스트림 세션을 시작합니다.
 * @param event Qt show 이벤트
 */
void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    updateDashboardAdaptiveSizes();
}

/**
 * @brief           로그인이 끝난 뒤 실제 수신을 시작합니다.
 * @param userName  인증된 사용자 이름
 * @param role      "admin" 또는 "operator"
 *
 * @details 로그인 창의 퇴장 연출과 겹쳐서 진행됩니다. RTSP handshake와 MQTT 접속에 걸리는
 *          1~2초의 앞부분을 그 애니메이션이 덮고, 나머지는 타일의 로딩 표시가 받습니다.
 */
void MainWindow::beginSession(const QString& userName, const QString& role) {
    if (streamSessionStarted_) {
        return;
    }
    streamSessionStarted_ = true;

    sessionUserName_ = userName;
    sessionIsAdmin_ = role == QStringLiteral("admin");
    qInfo().noquote() << QStringLiteral("[Session] %1 (%2) signed in").arg(userName, role);

    if (quickTopBar_ && quickTopBar_->rootObject()) {
        quickTopBar_->rootObject()->setProperty("settingsAllowed", sessionIsAdmin_);
    }

    if (deviceStatusService_) {
        deviceStatusService_->start();
    }

    QTimer::singleShot(videoConfig_.initialStartDelayMsec, this, [this]() {
        if (streamSessionManager_) {
            streamSessionManager_->start();
        }
    });
}

/**
 * @brief   현재 창 크기에 맞춰 대시보드 하단 영역과 객체 목록 폭을 보정합니다.
 */
void MainWindow::updateDashboardAdaptiveSizes() {
    DashboardLayout::adjustBottomSectionHeight(this, ui_.get());
    DashboardLayout::alignDeviceStatusCardWidth(ui_.get());
}

/**
 * @brief 4분할 영상 위젯의 더블클릭 이벤트를 확대/복구 동작에 연결합니다.
 */
void MainWindow::setupVideoViewEvents() {
    videoWidgets_ = {
        ui_->camView1, ui_->camView2, ui_->camView3, ui_->camView4,
        ui_->camView5, ui_->camView6, ui_->camView7, ui_->camView8,
    };

    videoAreaLayouts_ = {ui_->videoGridLayoutArea1, ui_->videoGridLayoutArea2};
    const qsizetype areaCount = videoConfig_.areas.size();
    const qsizetype channelCount = streamConfigs_.size();
    if (areaCount <= 0 || channelCount != areaCount * videoChannelsPerArea) {
        qWarning() << "[MainWindow] Invalid video area/channel configuration" << areaCount << channelCount;
        return;
    }

    while (videoAreaLayouts_.size() < areaCount) {
        auto* page = new QWidget(ui_->videoAreaStackedWidget);
        auto* grid = new QGridLayout(page);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(8);
        grid->setRowStretch(0, 1);
        grid->setRowStretch(1, 1);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        ui_->videoAreaStackedWidget->addWidget(page);
        videoAreaLayouts_.append(grid);

        for (int slotIndex = 0; slotIndex < videoChannelsPerArea; ++slotIndex) {
            videoWidgets_.append(new ClickableVideoWidget(page));
        }
    }

    videoAreaLayouts_.resize(areaCount);
    videoWidgets_.resize(channelCount);

    videoTileFrames_.resize(videoWidgets_.size());

    for (qsizetype areaIndex = 0; areaIndex < videoConfig_.areas.size(); ++areaIndex) {
        QGridLayout* grid = videoAreaLayouts_[areaIndex];
        const QVector<int>& channels = videoConfig_.areas[areaIndex].channelIndexes;
        if (!grid || channels.size() != videoChannelsPerArea) {
            qWarning() << "[MainWindow] Invalid video area layout" << areaIndex;
            continue;
        }

        for (qsizetype slotIndex = 0; slotIndex < channels.size(); ++slotIndex) {
            const int channelIndex = channels[slotIndex];
            QWidget* widget = videoWidgets_.value(channelIndex);
            auto* clickable = qobject_cast<ClickableVideoWidget*>(widget);
            if (!clickable) {
                qWarning() << "[MainWindow] Invalid video widget for channel" << channelIndex;
                continue;
            }

            clickable->setChannelName(
                QStringLiteral("CH %1").arg(videoLocalChannelNumber(channelIndex), 2, 10, QLatin1Char('0')));
            clickable->setExpandedView(false);
            for (QGridLayout* areaLayout : videoAreaLayouts_) {
                areaLayout->removeWidget(widget);
            }

            auto* tileFrame = new QFrame(grid->parentWidget());
            tileFrame->setObjectName(QStringLiteral("videoTileFrame"));
            tileFrame->setProperty("hovered", false);
            tileFrame->setProperty("riskLevel", QStringLiteral("normal"));
            tileFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

            auto* tileLayout = new QVBoxLayout(tileFrame);
            tileLayout->setContentsMargins(2, 2, 2, 2);
            tileLayout->setSpacing(0);
            tileLayout->addWidget(widget);

            grid->addWidget(tileFrame, static_cast<int>(slotIndex / 2), static_cast<int>(slotIndex % 2));
            videoTileFrames_[channelIndex] = tileFrame;

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
}

/** @brief 설정된 CCTV 구역을 상단 표시줄과 페이지 스택에 연결합니다. */
void MainWindow::setupVideoAreaSelector() {
    if (!ui_->videoAreaStackedWidget || videoConfig_.areas.isEmpty()) {
        return;
    }

    setQuickTopBarProperty("areaText", QStringLiteral("Zone %1").arg(currentVideoAreaIndex_ + 1));
    ui_->videoAreaStackedWidget->setCurrentIndex(currentVideoAreaIndex_);
}

/** @brief 현재 구역을 선택 상태로 표시한 뒤 구역 선택 다이얼로그를 엽니다. */
void MainWindow::openVideoAreaSelectionDialog() {
    QVariantList areaNames;
    areaNames.reserve(videoConfig_.areas.size());
    for (const VideoAreaConfig& area : videoConfig_.areas) {
        areaNames.append(area.name);
    }

    quickDialogMode_ = QuickDialogMode::AreaSelection;
    setQuickDialogProperty("choices", areaNames);
    setQuickDialogProperty("selectedIndex", currentVideoAreaIndex_);
    showQuickDialog(QStringLiteral("area"), QStringLiteral("모니터링 구역 선택"), QString());
}

/** @brief Qt Quick 오버레이의 확인 동작을 현재 모드에 따라 처리합니다. */
void MainWindow::handleQuickDialogAccepted(int selectedIndex) {
    const QuickDialogMode mode = quickDialogMode_;
    closeQuickDialog();

    if (mode == QuickDialogMode::AreaSelection) {
        switchVideoArea(selectedIndex);
    } else if (mode == QuickDialogMode::ReportConfirmation) {
        sendReport(pendingReportChannelNumber_);
    }
}

/** @brief 현재 Qt Quick 오버레이를 닫고 대기 모드를 초기화합니다. */
void MainWindow::closeQuickDialog() {
    if (quickDialogOverlay_) {
        quickDialogOverlay_->hide();
    }
    // 다음에 열 때 현재 선택이 곧바로 보이도록, 닫는 시점부터 전환을 꺼 둡니다.
    // showQuickDialog보다 먼저 selectedIndex가 설정되는 경로가 있어 여는 쪽에서 끄면 늦습니다.
    setQuickDialogProperty("selectionAnimated", false);
    quickDialogMode_ = QuickDialogMode::None;
}

/** @brief Qt Quick 오버레이의 속성을 루트 객체가 있을 때만 갱신합니다. */
void MainWindow::setQuickDialogProperty(const char* name, const QVariant& value) {
    if (quickDialogOverlay_ && quickDialogOverlay_->rootObject()) {
        quickDialogOverlay_->rootObject()->setProperty(name, value);
    }
}

/** @brief 지정한 문구와 모드로 Qt Quick 오버레이를 표시합니다. */
void MainWindow::showQuickDialog(const QString& mode, const QString& title, const QString& message) {
    if (!quickDialogOverlay_ || !quickDialogOverlay_->rootObject()) {
        return;
    }

    // 이전에 열었던 선택이 새 선택으로 흘러가는 전환이 보이지 않도록 여는 동안에는 끕니다.
    setQuickDialogProperty("selectionAnimated", false);
    setQuickDialogProperty("mode", mode);
    setQuickDialogProperty("titleText", title);
    setQuickDialogProperty("messageText", message);

    // QML이 알려준 패널 크기를 창 크기로 고정합니다. setGeometry만으로는 위젯 sizeHint에
    // 밀려 창이 작게 잡히면서 버튼이 잘렸습니다.
    QQuickItem* rootObject = quickDialogOverlay_->rootObject();
    const QSize panelSize(qRound(rootObject->implicitWidth()), qRound(rootObject->implicitHeight()));
    quickDialogOverlay_->setFixedSize(panelSize);
    const QWidget* host = ui_->centralwidget;
    const QPoint hostCenter = host->mapToGlobal(host->rect().center());
    quickDialogOverlay_->move(hostCenter - QPoint(panelSize.width() / 2, panelSize.height() / 2));
    quickDialogOverlay_->show();
    quickDialogOverlay_->raise();
    // 첫 프레임이 그려진 뒤에 되살려 여는 순간의 전환을 확실히 막습니다.
    QTimer::singleShot(150, this, [this]() { setQuickDialogProperty("selectionAnimated", true); });
}

/** @brief QML이 알려준 패널 크기로 오버레이 창을 메인 윈도우 중앙에 맞춥니다. */
QRect MainWindow::quickDialogHostGeometry() const {
    const QQuickItem* rootObject = quickDialogOverlay_->rootObject();
    const QSize panelSize(qRound(rootObject->implicitWidth()), qRound(rootObject->implicitHeight()));
    const QWidget* host = ui_->centralwidget;
    const QPoint hostCenter = host->mapToGlobal(host->rect().center());
    return QRect(hostCenter - QPoint(panelSize.width() / 2, panelSize.height() / 2), panelSize);
}

/**
 * @brief              전역 채널이 속한 영상 구역 인덱스를 찾습니다.
 * @param channelIndex 0 기반 전역 채널 인덱스
 * @return             구역 인덱스 또는 -1
 */
int MainWindow::videoAreaIndexForChannel(int channelIndex) const {
    for (qsizetype areaIndex = 0; areaIndex < videoConfig_.areas.size(); ++areaIndex) {
        if (videoConfig_.areas[areaIndex].channelIndexes.contains(channelIndex)) {
            return static_cast<int>(areaIndex);
        }
    }
    return -1;
}

/**
 * @brief           구역에 배정된 전역 채널 인덱스를 반환합니다.
 * @param areaIndex 조회할 구역 인덱스
 * @return           화면 슬롯 순서의 채널 인덱스
 */
const QVector<int>& MainWindow::channelsForArea(int areaIndex) const {
    static const QVector<int> emptyChannels;
    if (areaIndex < 0 || areaIndex >= videoConfig_.areas.size()) {
        return emptyChannels;
    }
    return videoConfig_.areas[areaIndex].channelIndexes;
}

/** @brief 지정한 전역 채널이 지금 화면에 실제로 그려지고 있는지 확인합니다. */
bool MainWindow::isChannelVisible(int channelIndex) const {
    // 확대 중에는 그 채널 하나만 화면에 있다. 나머지 타일은 hide된 상태라 디코딩할 이유가 없다
    if (expandedWidget_) {
        return videoWidgets_.value(channelIndex) == expandedWidget_;
    }

    return channelsForArea(currentVideoAreaIndex_).contains(channelIndex);
}

/**
 * @brief 현재 표시 상태에 맞춰 채널별 디코딩 활성 여부를 갱신합니다.
 *
 * @details presentation valve는 디코더 앞에 있으므로, 꺼진 채널은 디코드·블러·GPU 업로드를
 *          통째로 건너뛴다. 표시 상태를 바꾸는 곳(구역 전환, 확대, 복구)은 모두 이 함수를 불러
 *          한 가지 규칙(isChannelVisible)만 따르게 한다. 켜기를 먼저 돌리고 끄기를 나중에 돌려
 *          전환 중에 아무 채널도 표시되지 않는 구간이 생기지 않게 한다.
 */
void MainWindow::syncStreamPresentation() {
    if (!streamSessionManager_) {
        return;
    }

    for (const bool active : {true, false}) {
        for (const StreamConfig& stream : streamConfigs_) {
            if (isChannelVisible(stream.channelIndex) == active) {
                streamSessionManager_->setPresentationActive(stream.channelIndex, active);
            }
        }
    }
}

/**
 * @brief           워밍 스트림을 유지한 채 CCTV 표시 구역을 전환합니다.
 * @param areaIndex 새로 표시할 구역 인덱스
 */
void MainWindow::switchVideoArea(int areaIndex) {
    if (areaIndex < 0 || areaIndex >= videoConfig_.areas.size() || areaIndex == currentVideoAreaIndex_) {
        return;
    }

    if (expandedWidget_) {
        restoreVideoGrid();
    }

    currentVideoAreaIndex_ = areaIndex;
    syncStreamPresentation();
    ui_->videoAreaStackedWidget->setCurrentIndex(areaIndex);
    setQuickTopBarProperty("areaText", QStringLiteral("Zone %1").arg(currentVideoAreaIndex_ + 1));
    if (deviceStatusPanel_) {
        deviceStatusPanel_->setAreaIndex(areaIndex);
    }
    if (!isChannelVisible(selectedPreprocessingChannelIndex_)) {
        selectedPreprocessingChannelIndex_ = channelsForArea(areaIndex).value(0, 0);
    }
    updateVideoRiskBorders(latestVideoRiskLevels_);
    updateStreamConnectionStatus();

    for (int channelIndex : channelsForArea(areaIndex)) {
        if (auto* videoWidget = qobject_cast<ClickableVideoWidget*>(videoWidgets_.value(channelIndex))) {
            videoWidget->refreshChannelLabel();
        }
    }
}

/**
 * @brief             디지털 트윈의 채널별 위험 상태를 CCTV 타일 테두리에
 * 반영합니다.
 * @param riskLevels  설정된 전역 채널 순서의 현재 위험 단계
 */
void MainWindow::updateVideoRiskBorders(const QVector<DigitalTwinRiskLevel>& riskLevels) {
    if (latestVideoRiskLevels_.size() != streamConfigs_.size()) {
        latestVideoRiskLevels_.fill(DigitalTwinRiskLevel::Normal, streamConfigs_.size());
    }

    const qsizetype count = qMin(latestVideoRiskLevels_.size(), riskLevels.size());
    for (qsizetype channelIndex = 0; channelIndex < count; ++channelIndex) {
        latestVideoRiskLevels_[channelIndex] = riskLevels[channelIndex];
    }

    for (qsizetype channelIndex = 0; channelIndex < videoTileFrames_.size(); ++channelIndex) {
        QFrame* tileFrame = videoTileFrames_[channelIndex];
        if (!tileFrame) {
            continue;
        }

        const DigitalTwinRiskLevel visibleRiskLevel =
            videoRiskBordersEnabled_ ? latestVideoRiskLevels_.value(channelIndex) : DigitalTwinRiskLevel::Normal;

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
        const bool hovered = riskName == QStringLiteral("normal") && videoWidgets_[channelIndex] &&
                             videoWidgets_[channelIndex]->underMouse();
        tileFrame->setProperty("hovered", hovered);
        tileFrame->style()->unpolish(tileFrame);
        tileFrame->style()->polish(tileFrame);
        tileFrame->update();
    }
}

/**
 * @brief                  영상 출력 창과 스트림 설정을 세션 관리자에
 * 연결합니다.
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
    streamSessionManager_->setVideoPreprocessingSettings(videoConfig_.receiver.preprocessing);
    syncStreamPresentation();

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
 * @brief              현재 확대 상태에 따라 대상 영상을 확대하거나 4분할로
 * 복구합니다.
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
    const qsizetype targetIndex = videoWidgets_.indexOf(targetWidget);
    if (targetIndex < 0 || targetIndex >= videoTileFrames_.size()) {
        qWarning() << "[MainWindow] Video tile frame is not configured";
        return;
    }

    const int areaIndex = videoAreaIndexForChannel(static_cast<int>(targetIndex));
    QGridLayout* grid = videoAreaLayouts_.value(areaIndex);
    if (!grid || areaIndex != currentVideoAreaIndex_) {
        qWarning() << "[MainWindow] Video area layout is not available" << areaIndex;
        return;
    }

    QFrame* targetFrame = videoTileFrames_[targetIndex];

    for (auto* widget : videoWidgets_) {
        if (auto* videoWidget = qobject_cast<ClickableVideoWidget*>(widget)) {
            videoWidget->setExpandedView(widget == targetWidget);
        }
    }

    for (int channelIndex : channelsForArea(areaIndex)) {
        QFrame* frame = videoTileFrames_.value(channelIndex);
        if (frame && frame != targetFrame) {
            frame->hide();
        }
    }

    grid->removeWidget(targetFrame);
    grid->addWidget(targetFrame, 0, 0, 2, 2);

    targetFrame->show();
    targetFrame->raise();

    expandedWidget_ = targetWidget;
    syncStreamPresentation();
}

/**
 * @brief   확대된 영상을 원래 2x2 영상 그리드로 복구합니다.
 */
void MainWindow::restoreVideoGrid() {
    if (!expandedWidget_) {
        return;
    }

    const int expandedChannelIndex = static_cast<int>(videoWidgets_.indexOf(expandedWidget_));
    const int areaIndex = videoAreaIndexForChannel(expandedChannelIndex);
    QGridLayout* grid = videoAreaLayouts_.value(areaIndex);
    if (!grid) {
        qWarning() << "[MainWindow] Video area layout is not available" << areaIndex;
        return;
    }

    const QVector<int>& channels = channelsForArea(areaIndex);
    for (int channelIndex : channels) {
        QFrame* frame = videoTileFrames_.value(channelIndex);
        if (frame) {
            grid->removeWidget(frame);
        }
    }

    for (qsizetype slotIndex = 0; slotIndex < channels.size(); ++slotIndex) {
        QFrame* frame = videoTileFrames_.value(channels[slotIndex]);
        if (frame) {
            grid->addWidget(frame, static_cast<int>(slotIndex / 2), static_cast<int>(slotIndex % 2));
        }
    }

    for (int channelIndex : channels) {
        QFrame* frame = videoTileFrames_.value(channelIndex);
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
    syncStreamPresentation();
}
