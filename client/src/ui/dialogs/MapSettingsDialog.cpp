#include "ui/dialogs/MapSettingsDialog.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/dialogs/InformationDialog.h"

namespace {
constexpr int dialogPanelWidth = 800;
constexpr int dialogPanelHeight = 600;
constexpr int preprocessingChannelCount = 4;
constexpr int customPresetIndex = 0;
constexpr int dayPresetIndex = 1;
constexpr int nightPresetIndex = 2;

class MapOptionCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;

protected:
    /**
     * @brief       선택 상태에 선명한 체크 표시를 직접 그립니다.
     * @param event Qt 그리기 이벤트
     */
    void paintEvent(QPaintEvent* event) override {
        QCheckBox::paintEvent(event);
        if (!isChecked()) {
            return;
        }

        QStyleOptionButton option;
        initStyleOption(&option);
        const QRect indicatorRect = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(QStringLiteral("#ffffff")), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

        QPolygonF checkMark;
        checkMark << QPointF(indicatorRect.left() + 5.0, indicatorRect.center().y())
                  << QPointF(indicatorRect.left() + 10.0, indicatorRect.bottom() - 5.0)
                  << QPointF(indicatorRect.right() - 4.0, indicatorRect.top() + 5.0);
        painter.drawPolyline(checkMark);
    }
};

class SettingsTabWidget final : public QTabWidget {
public:
    using QTabWidget::QTabWidget;

protected:
    /**
     * @brief       탭 바를 설정 내용 영역과 같은 너비로 유지합니다.
     * @param event Qt 크기 변경 이벤트
     */
    void resizeEvent(QResizeEvent* event) override {
        QTabWidget::resizeEvent(event);
        tabBar()->setFixedWidth(width());
    }
};

class VideoOptionComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    /**
     * @brief       플랫폼별 기본 화살표 대신 테마와 맞는 화살표를 그립니다.
     * @param event Qt 그리기 이벤트
     */
    void paintEvent(QPaintEvent* event) override {
        QComboBox::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor arrowColor = isEnabled() ? QColor(QStringLiteral("#8edfff")) : QColor(QStringLiteral("#4a6b7d"));
        painter.setPen(QPen(arrowColor, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

        const qreal centerX = width() - 17.0;
        const qreal centerY = height() / 2.0;
        QPolygonF chevron;
        chevron << QPointF(centerX - 4.0, centerY - 2.0) << QPointF(centerX, centerY + 2.0)
                << QPointF(centerX + 4.0, centerY - 2.0);
        painter.drawPolyline(chevron);
    }
};

/**
 * @brief        설정창 공통 스타일을 사용하는 체크박스를 생성합니다.
 * @param text   체크박스 표시 문구
 * @param parent 체크박스 부모 위젯
 * @return       생성된 체크박스
 */
QCheckBox* createOptionCheckBox(const QString& text, QWidget* parent) {
    auto* checkBox = new MapOptionCheckBox(text, parent);
    checkBox->setObjectName(QStringLiteral("mapSettingsOption"));
    checkBox->setCursor(Qt::PointingHandCursor);
    return checkBox;
}

/**
 * @brief            영상 조정 슬라이더 한 행을 구성합니다.
 * @param layout     컨트롤을 배치할 그리드 레이아웃
 * @param row        배치할 행 인덱스
 * @param title      조정 항목 이름
 * @param minimum    슬라이더 최솟값
 * @param maximum    슬라이더 최댓값
 * @param slider     생성된 슬라이더를 받을 포인터
 * @param valueLabel 현재 값을 표시할 라벨 포인터
 * @param parent     컨트롤의 부모 위젯
 */
void addSliderRow(QGridLayout* layout, int row, const QString& title, int minimum, int maximum, QSlider*& slider,
                  QLabel*& valueLabel, QWidget* parent) {
    auto* titleLabel = new QLabel(title, parent);
    titleLabel->setObjectName(QStringLiteral("videoPreprocessingFieldLabel"));

    slider = new QSlider(Qt::Horizontal, parent);
    slider->setObjectName(QStringLiteral("videoPreprocessingSlider"));
    slider->setRange(minimum, maximum);
    slider->setCursor(Qt::PointingHandCursor);

    valueLabel = new QLabel(parent);
    valueLabel->setObjectName(QStringLiteral("videoPreprocessingValueLabel"));
    valueLabel->setAlignment(Qt::AlignCenter);
    valueLabel->setFixedWidth(64);

    layout->addWidget(titleLabel, row, 0);
    layout->addWidget(slider, row, 1);
    layout->addWidget(valueLabel, row, 2);
}
}  // namespace

/**
 * @brief        UI 표시와 영상 전처리 옵션을 편집하는 모달 팝업을 구성합니다.
 * @param parent 팝업 배경을 덮을 메인 윈도우
 */
MapSettingsDialog::MapSettingsDialog(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("mapSettingsDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);

    informationDialog_ = new InformationDialog(this);

    auto* overlayLayout = new QVBoxLayout(this);
    overlayLayout->setContentsMargins(0, 0, 0, 0);
    overlayLayout->setAlignment(Qt::AlignCenter);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("mapSettingsDialogPanel"));
    panel->setFixedSize(dialogPanelWidth, dialogPanelHeight);
    overlayLayout->addWidget(panel);

    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(32, 22, 32, 22);
    panelLayout->setSpacing(12);

    auto* headerLayout = new QHBoxLayout();
    auto* titleLabel = new QLabel(QStringLiteral("설정"), panel);
    titleLabel->setObjectName(QStringLiteral("mapSettingsTitleLabel"));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch(1);

    auto* closeButton = new QPushButton(QStringLiteral("×"), panel);
    closeButton->setObjectName(QStringLiteral("mapSettingsCloseButton"));
    closeButton->setToolTip(QStringLiteral("닫기"));
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, this, &QWidget::hide);
    headerLayout->addWidget(closeButton);
    panelLayout->addLayout(headerLayout);

    auto* settingsTabs = new SettingsTabWidget(panel);
    settingsTabs->setObjectName(QStringLiteral("settingsTabs"));
    settingsTabs->setUsesScrollButtons(false);
    settingsTabs->tabBar()->setExpanding(true);
    settingsTabs->tabBar()->setCursor(Qt::PointingHandCursor);
    panelLayout->addWidget(settingsTabs, 1);

    auto* uiTab = new QWidget(settingsTabs);
    uiTab->setObjectName(QStringLiteral("settingsTabPage"));
    auto* uiTabLayout = new QVBoxLayout(uiTab);
    uiTabLayout->setContentsMargins(0, 0, 0, 0);

    auto* optionsFrame = new QFrame(uiTab);
    optionsFrame->setObjectName(QStringLiteral("mapSettingsOptionsFrame"));
    auto* optionsLayout = new QVBoxLayout(optionsFrame);
    optionsLayout->setContentsMargins(28, 22, 28, 24);
    optionsLayout->setSpacing(18);

    auto* sectionTitleLabel = new QLabel(QStringLiteral("맵 표시 설정"), optionsFrame);
    sectionTitleLabel->setObjectName(QStringLiteral("mapSettingsSectionLabel"));
    optionsLayout->addWidget(sectionTitleLabel);

    auto* optionGrid = new QGridLayout();
    optionGrid->setHorizontalSpacing(72);
    optionGrid->setVerticalSpacing(18);
    optionGrid->setColumnStretch(0, 1);
    optionGrid->setColumnStretch(1, 1);

    movementTrailsCheckBox_ = createOptionCheckBox(QStringLiteral("이동 경로 표시"), optionsFrame);
    ledCheckBox_ = createOptionCheckBox(QStringLiteral("LED 표시"), optionsFrame);
    cctvCheckBox_ = createOptionCheckBox(QStringLiteral("CCTV 표시"), optionsFrame);
    alertDeviceCheckBox_ = createOptionCheckBox(QStringLiteral("알림 장치 표시"), optionsFrame);
    optionGrid->addWidget(movementTrailsCheckBox_, 0, 0);
    optionGrid->addWidget(ledCheckBox_, 0, 1);
    optionGrid->addWidget(cctvCheckBox_, 1, 0);
    optionGrid->addWidget(alertDeviceCheckBox_, 1, 1);
    optionsLayout->addLayout(optionGrid);

    auto* horizontalDivider = new QFrame(optionsFrame);
    horizontalDivider->setObjectName(QStringLiteral("mapSettingsHorizontalDivider"));
    horizontalDivider->setFrameShape(QFrame::HLine);
    optionsLayout->addWidget(horizontalDivider);

    auto* secondaryOptionsLayout = new QHBoxLayout();
    secondaryOptionsLayout->setContentsMargins(0, 0, 0, 0);
    secondaryOptionsLayout->setSpacing(32);

    auto* cctvOptionsLayout = new QVBoxLayout();
    cctvOptionsLayout->setSpacing(14);
    auto* cctvSectionTitleLabel = new QLabel(QStringLiteral("CCTV 알림 설정"), optionsFrame);
    cctvSectionTitleLabel->setObjectName(QStringLiteral("mapSettingsSectionLabel"));
    cctvOptionsLayout->addWidget(cctvSectionTitleLabel);
    videoRiskBordersCheckBox_ = createOptionCheckBox(QStringLiteral("CCTV 테두리 알림 표시"), optionsFrame);
    cctvOptionsLayout->addWidget(videoRiskBordersCheckBox_);
    cctvOptionsLayout->addStretch(1);
    secondaryOptionsLayout->addLayout(cctvOptionsLayout, 1);

    auto* sectionDivider = new QFrame(optionsFrame);
    sectionDivider->setObjectName(QStringLiteral("mapSettingsSectionDivider"));
    sectionDivider->setFrameShape(QFrame::VLine);
    secondaryOptionsLayout->addWidget(sectionDivider);

    auto* blurOptionsLayout = new QVBoxLayout();
    blurOptionsLayout->setSpacing(14);
    auto* blurSectionTitleLabel = new QLabel(QStringLiteral("블러 설정"), optionsFrame);
    blurSectionTitleLabel->setObjectName(QStringLiteral("mapSettingsSectionLabel"));
    blurOptionsLayout->addWidget(blurSectionTitleLabel);

    auto* blurOptionLayout = new QHBoxLayout();
    blurOptionLayout->setSpacing(28);
    faceBlurCheckBox_ = createOptionCheckBox(QStringLiteral("얼굴"), optionsFrame);
    licensePlateBlurCheckBox_ = createOptionCheckBox(QStringLiteral("차량 번호판"), optionsFrame);
    blurOptionLayout->addWidget(faceBlurCheckBox_);
    blurOptionLayout->addWidget(licensePlateBlurCheckBox_);
    blurOptionLayout->addStretch(1);
    blurOptionsLayout->addLayout(blurOptionLayout);
    blurOptionsLayout->addStretch(1);
    secondaryOptionsLayout->addLayout(blurOptionsLayout, 1);

    optionsLayout->addLayout(secondaryOptionsLayout, 1);
    uiTabLayout->addWidget(optionsFrame, 1);
    settingsTabs->addTab(uiTab, QStringLiteral("UI 설정"));

    auto* videoTab = new QWidget(settingsTabs);
    videoTab->setObjectName(QStringLiteral("settingsTabPage"));
    auto* videoTabLayout = new QVBoxLayout(videoTab);
    videoTabLayout->setContentsMargins(0, 0, 0, 0);

    auto* preprocessingFrame = new QFrame(videoTab);
    preprocessingFrame->setObjectName(QStringLiteral("mapSettingsOptionsFrame"));
    auto* preprocessingLayout = new QVBoxLayout(preprocessingFrame);
    preprocessingLayout->setContentsMargins(28, 16, 28, 18);
    preprocessingLayout->setSpacing(10);

    auto* preprocessingHeaderLayout = new QHBoxLayout();
    auto* preprocessingTitleLabel = new QLabel(QStringLiteral("영상 전처리"), preprocessingFrame);
    preprocessingTitleLabel->setObjectName(QStringLiteral("mapSettingsSectionLabel"));
    preprocessingHeaderLayout->addWidget(preprocessingTitleLabel);
    preprocessingHeaderLayout->addStretch(1);
    preprocessingEnabledCheckBox_ = createOptionCheckBox(QStringLiteral("사용"), preprocessingFrame);
    preprocessingHeaderLayout->addWidget(preprocessingEnabledCheckBox_);
    preprocessingLayout->addLayout(preprocessingHeaderLayout);

    auto* channelLayout = new QHBoxLayout();
    auto* channelLabel = new QLabel(QStringLiteral("적용 대상"), preprocessingFrame);
    channelLabel->setObjectName(QStringLiteral("videoPreprocessingFieldLabel"));
    channelLayout->addWidget(channelLabel);
    channelLayout->addStretch(1);
    preprocessingChannelComboBox_ = new VideoOptionComboBox(preprocessingFrame);
    preprocessingChannelComboBox_->setObjectName(QStringLiteral("videoPreprocessingChannelComboBox"));
    preprocessingChannelComboBox_->addItems(
        {QStringLiteral("CH 01"), QStringLiteral("CH 02"), QStringLiteral("CH 03"), QStringLiteral("CH 04")});
    preprocessingChannelComboBox_->setCursor(Qt::PointingHandCursor);
    preprocessingChannelComboBox_->setMinimumWidth(190);
    channelLayout->addWidget(preprocessingChannelComboBox_);
    preprocessingLayout->addLayout(channelLayout);

    preprocessingControlsWidget_ = new QWidget(preprocessingFrame);
    preprocessingControlsWidget_->setObjectName(QStringLiteral("videoPreprocessingControls"));
    auto* controlsLayout = new QVBoxLayout(preprocessingControlsWidget_);
    controlsLayout->setContentsMargins(0, 4, 0, 0);
    controlsLayout->setSpacing(10);

    auto* presetLayout = new QHBoxLayout();
    auto* presetLabel = new QLabel(QStringLiteral("모드"), preprocessingControlsWidget_);
    presetLabel->setObjectName(QStringLiteral("videoPreprocessingFieldLabel"));
    presetLayout->addWidget(presetLabel);
    presetLayout->addStretch(1);
    preprocessingPresetComboBox_ = new VideoOptionComboBox(preprocessingControlsWidget_);
    preprocessingPresetComboBox_->setObjectName(QStringLiteral("videoPreprocessingComboBox"));
    preprocessingPresetComboBox_->addItems(
        {QStringLiteral("사용자 설정"), QStringLiteral("주간"), QStringLiteral("야간")});
    preprocessingPresetComboBox_->setCursor(Qt::PointingHandCursor);
    preprocessingPresetComboBox_->setMinimumWidth(190);
    presetLayout->addWidget(preprocessingPresetComboBox_);
    controlsLayout->addLayout(presetLayout);
    controlsLayout->addSpacing(10);

    auto* adjustmentGrid = new QGridLayout();
    adjustmentGrid->setHorizontalSpacing(18);
    adjustmentGrid->setVerticalSpacing(12);
    adjustmentGrid->setColumnStretch(1, 1);
    addSliderRow(adjustmentGrid, 0, QStringLiteral("밝기"), -20, 20, brightnessSlider_, brightnessValueLabel_,
                 preprocessingControlsWidget_);
    addSliderRow(adjustmentGrid, 1, QStringLiteral("대비"), 80, 120, contrastSlider_, contrastValueLabel_,
                 preprocessingControlsWidget_);
    addSliderRow(adjustmentGrid, 2, QStringLiteral("감마"), 80, 140, gammaSlider_, gammaValueLabel_,
                 preprocessingControlsWidget_);
    controlsLayout->addLayout(adjustmentGrid);

    controlsLayout->addStretch(1);
    preprocessingLayout->addWidget(preprocessingControlsWidget_, 1);

    auto* preprocessingButtonLayout = new QHBoxLayout();
    auto* resetButton = new QPushButton(QStringLiteral("기본값 복원"), preprocessingFrame);
    resetButton->setObjectName(QStringLiteral("videoPreprocessingSecondaryButton"));
    resetButton->setCursor(Qt::PointingHandCursor);
    preprocessingButtonLayout->addWidget(resetButton);
    preprocessingButtonLayout->addStretch(1);
    auto* applySelectedButton = new QPushButton(QStringLiteral("선택 채널 적용"), preprocessingFrame);
    applySelectedButton->setObjectName(QStringLiteral("videoPreprocessingSecondaryButton"));
    applySelectedButton->setCursor(Qt::PointingHandCursor);
    preprocessingButtonLayout->addWidget(applySelectedButton);
    auto* applyAllButton = new QPushButton(QStringLiteral("전체 채널 적용"), preprocessingFrame);
    applyAllButton->setObjectName(QStringLiteral("videoPreprocessingApplyAllButton"));
    applyAllButton->setCursor(Qt::PointingHandCursor);
    preprocessingButtonLayout->addWidget(applyAllButton);
    preprocessingLayout->addLayout(preprocessingButtonLayout);

    videoTabLayout->addWidget(preprocessingFrame, 1);
    settingsTabs->addTab(videoTab, QStringLiteral("영상 설정"));

    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("취소"), panel);
    cancelButton->setObjectName(QStringLiteral("mapSettingsCancelButton"));
    cancelButton->setCursor(Qt::PointingHandCursor);
    connect(cancelButton, &QPushButton::clicked, this, &QWidget::hide);
    buttonLayout->addWidget(cancelButton);

    auto* applyButton = new QPushButton(QStringLiteral("적용"), panel);
    applyButton->setObjectName(QStringLiteral("mapSettingsApplyButton"));
    applyButton->setCursor(Qt::PointingHandCursor);
    applyButton->setDefault(true);
    connect(applyButton, &QPushButton::clicked, this, [this]() {
        storeCurrentPreprocessingChannel();
        emit settingsApplied(settings(), videoRiskBordersEnabled(), faceBlurEnabled(), licensePlateBlurEnabled(),
                             currentPreprocessingChannelIndex_,
                             preprocessingSettingsByChannel_.value(currentPreprocessingChannelIndex_));
        hide();
    });
    buttonLayout->addWidget(applyButton);
    panelLayout->addLayout(buttonLayout);

    connect(preprocessingEnabledCheckBox_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!enabled && !updatingPreprocessingControls_) {
            VideoPreprocessingSettings settings;
            settings.enabled = false;
            setPreprocessingControls(settings);
            return;
        }
        setPreprocessingControlsEnabled(enabled);
    });
    connect(preprocessingChannelComboBox_, &QComboBox::currentIndexChanged, this, [this](int channelIndex) {
        if (updatingPreprocessingControls_) {
            return;
        }
        storeCurrentPreprocessingChannel();
        currentPreprocessingChannelIndex_ = channelIndex;
        loadPreprocessingChannel(channelIndex);
    });
    connect(preprocessingPresetComboBox_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updatingPreprocessingControls_) {
            return;
        }
        if (index == dayPresetIndex) {
            applyPreprocessingPreset(VideoPreprocessingPreset::Day);
        } else if (index == nightPresetIndex) {
            applyPreprocessingPreset(VideoPreprocessingPreset::Night);
        }
    });

    const auto handleManualAdjustment = [this]() {
        updatePreprocessingValueLabels();
        markPreprocessingAsCustom();
    };
    connect(brightnessSlider_, &QSlider::valueChanged, this, handleManualAdjustment);
    connect(contrastSlider_, &QSlider::valueChanged, this, handleManualAdjustment);
    connect(gammaSlider_, &QSlider::valueChanged, this, handleManualAdjustment);
    connect(resetButton, &QPushButton::clicked, this,
            [this]() { applyPreprocessingPreset(VideoPreprocessingPreset::Custom); });
    connect(applySelectedButton, &QPushButton::clicked, this, [this]() {
        storeCurrentPreprocessingChannel();
        emit videoPreprocessingApplyRequested(currentPreprocessingChannelIndex_,
                                              preprocessingSettingsByChannel_.value(currentPreprocessingChannelIndex_));
        showPreprocessingAppliedMessage(
            QStringLiteral("%1 채널에 변경된 설정이 적용되었습니다!").arg(currentPreprocessingChannelIndex_ + 1));
    });
    connect(applyAllButton, &QPushButton::clicked, this, [this]() {
        const VideoPreprocessingSettings settings = videoPreprocessingSettings();
        preprocessingSettingsByChannel_.fill(settings, preprocessingChannelCount);
        emit videoPreprocessingApplyRequested(-1, settings);
        showPreprocessingAppliedMessage(QStringLiteral("전체 채널에 변경된 설정이 적용되었습니다!"));
    });

    preprocessingSettingsByChannel_.fill(VideoPreprocessingSettings{}, preprocessingChannelCount);
    setVideoPreprocessingSettings(preprocessingSettingsByChannel_);
    hide();
}

/**
 * @brief          현재 맵 표시 설정을 체크 항목에 반영합니다.
 * @param settings 편집을 시작할 맵 표시 설정
 */
void MapSettingsDialog::setSettings(const DigitalTwinMapDisplaySettings& settings) {
    movementTrailsCheckBox_->setChecked(settings.showMovementTrails);
    ledCheckBox_->setChecked(settings.showLed);
    cctvCheckBox_->setChecked(settings.showCctv);
    alertDeviceCheckBox_->setChecked(settings.showAlertDevice);
}

/**
 * @brief         CCTV 경고·위험 테두리 알림의 체크 상태를 설정합니다.
 * @param enabled 테두리 알림 표시 여부
 */
void MapSettingsDialog::setVideoRiskBordersEnabled(bool enabled) { videoRiskBordersCheckBox_->setChecked(enabled); }

/**
 * @brief                     얼굴·차량 번호판 블러의 체크 상태를 설정합니다.
 * @param faceEnabled         얼굴 블러 활성화 여부
 * @param licensePlateEnabled 차량 번호판 블러 활성화 여부
 */
void MapSettingsDialog::setBlurTargetsEnabled(bool faceEnabled, bool licensePlateEnabled) {
    faceBlurCheckBox_->setChecked(faceEnabled);
    licensePlateBlurCheckBox_->setChecked(licensePlateEnabled);
}

/**
 * @brief                      채널별 영상 전처리 설정을 영상 탭에 반영합니다.
 * @param settingsByChannel    채널 인덱스 순서의 영상 전처리 설정
 * @param selectedChannelIndex 처음 표시할 채널 인덱스
 */
void MapSettingsDialog::setVideoPreprocessingSettings(const QVector<VideoPreprocessingSettings>& settingsByChannel,
                                                      int selectedChannelIndex) {
    preprocessingSettingsByChannel_ = settingsByChannel;
    if (preprocessingSettingsByChannel_.size() < preprocessingChannelCount) {
        preprocessingSettingsByChannel_.resize(preprocessingChannelCount);
    }

    currentPreprocessingChannelIndex_ = qBound(0, selectedChannelIndex, preprocessingSettingsByChannel_.size() - 1);

    const QSignalBlocker channelBlocker(preprocessingChannelComboBox_);
    preprocessingChannelComboBox_->setCurrentIndex(currentPreprocessingChannelIndex_);
    loadPreprocessingChannel(currentPreprocessingChannelIndex_);
}

/**
 * @brief          영상 전처리 설정 하나를 현재 컨트롤에 반영합니다.
 * @param settings 표시할 영상 전처리 설정
 */
void MapSettingsDialog::setPreprocessingControls(const VideoPreprocessingSettings& settings) {
    updatingPreprocessingControls_ = true;
    preprocessingEnabledCheckBox_->setChecked(settings.enabled);
    brightnessSlider_->setValue(settings.brightness);
    contrastSlider_->setValue(qRound(settings.contrast * 100.0));
    gammaSlider_->setValue(qRound(settings.gamma * 100.0));

    int presetIndex = customPresetIndex;
    if (settings.preset == VideoPreprocessingPreset::Day) {
        presetIndex = dayPresetIndex;
    } else if (settings.preset == VideoPreprocessingPreset::Night) {
        presetIndex = nightPresetIndex;
    }
    preprocessingPresetComboBox_->setCurrentIndex(presetIndex);
    updatingPreprocessingControls_ = false;

    updatePreprocessingValueLabels();
    setPreprocessingControlsEnabled(settings.enabled);
}

/**
 * @brief  사용자가 선택한 네 개 표시 옵션을 반환합니다.
 * @return 현재 체크 상태로 구성한 맵 표시 설정
 */
DigitalTwinMapDisplaySettings MapSettingsDialog::settings() const {
    DigitalTwinMapDisplaySettings displaySettings;
    displaySettings.showMovementTrails = movementTrailsCheckBox_->isChecked();
    displaySettings.showLed = ledCheckBox_->isChecked();
    displaySettings.showCctv = cctvCheckBox_->isChecked();
    displaySettings.showAlertDevice = alertDeviceCheckBox_->isChecked();
    return displaySettings;
}

/**
 * @brief  CCTV 테두리 알림 표시 여부를 반환합니다.
 * @return 테두리 알림을 표시하면 true
 */
bool MapSettingsDialog::videoRiskBordersEnabled() const { return videoRiskBordersCheckBox_->isChecked(); }

/**
 * @brief  얼굴 블러 표시 여부를 반환합니다.
 * @return 얼굴 블러가 활성화되어 있으면 true
 */
bool MapSettingsDialog::faceBlurEnabled() const { return faceBlurCheckBox_->isChecked(); }

/**
 * @brief  차량 번호판 블러 표시 여부를 반환합니다.
 * @return 차량 번호판 블러가 활성화되어 있으면 true
 */
bool MapSettingsDialog::licensePlateBlurEnabled() const { return licensePlateBlurCheckBox_->isChecked(); }

/**
 * @brief  현재 영상 탭 값을 전처리 설정 모델로 변환합니다.
 * @return 모든 채널 수신기에 전달할 영상 전처리 설정
 */
VideoPreprocessingSettings MapSettingsDialog::videoPreprocessingSettings() const {
    VideoPreprocessingSettings settings;
    settings.enabled = preprocessingEnabledCheckBox_->isChecked();
    if (!settings.enabled) {
        settings.enabled = false;
        return settings;
    }

    settings.brightness = brightnessSlider_->value();
    settings.contrast = static_cast<double>(contrastSlider_->value()) / 100.0;
    settings.gamma = static_cast<double>(gammaSlider_->value()) / 100.0;

    if (preprocessingPresetComboBox_->currentIndex() == dayPresetIndex) {
        settings.preset = VideoPreprocessingPreset::Day;
    } else if (preprocessingPresetComboBox_->currentIndex() == nightPresetIndex) {
        settings.preset = VideoPreprocessingPreset::Night;
    }
    return settings;
}

/**
 * @brief        주간·야간 또는 중립 기본값을 영상 설정 컨트롤에 적용합니다.
 * @param preset 적용할 프리셋
 */
void MapSettingsDialog::applyPreprocessingPreset(VideoPreprocessingPreset preset) {
    VideoPreprocessingSettings settings;
    settings.enabled = preset == VideoPreprocessingPreset::Custom || preprocessingEnabledCheckBox_->isChecked();
    settings.preset = preset;

    if (preset == VideoPreprocessingPreset::Day) {
        settings.brightness = 3;
        settings.contrast = 1.08;
        settings.gamma = 1.0;
    } else if (preset == VideoPreprocessingPreset::Night) {
        settings.brightness = 10;
        settings.contrast = 1.05;
        settings.gamma = 1.2;
    }

    setPreprocessingControls(settings);
}

/**
 * @brief              선택한 채널의 저장된 영상 전처리 설정을 불러옵니다.
 * @param channelIndex 불러올 0 기반 채널 인덱스
 */
void MapSettingsDialog::loadPreprocessingChannel(int channelIndex) {
    if (channelIndex < 0 || channelIndex >= preprocessingSettingsByChannel_.size()) {
        return;
    }
    setPreprocessingControls(preprocessingSettingsByChannel_.at(channelIndex));
}

/**
 * @brief 현재 컨트롤 값을 선택 중인 채널 설정에 저장합니다.
 */
void MapSettingsDialog::storeCurrentPreprocessingChannel() {
    if (currentPreprocessingChannelIndex_ < 0 ||
        currentPreprocessingChannelIndex_ >= preprocessingSettingsByChannel_.size()) {
        return;
    }
    preprocessingSettingsByChannel_[currentPreprocessingChannelIndex_] = videoPreprocessingSettings();
}

/**
 * @brief         영상 전처리 설정 전달 완료 안내를 비동기 모달로 표시합니다.
 * @param message 선택 또는 전체 채널 적용 결과 문구
 */
void MapSettingsDialog::showPreprocessingAppliedMessage(const QString& message) {
    if (!informationDialog_) {
        return;
    }

    informationDialog_->setContent(QStringLiteral("영상 설정 적용"), message);
    informationDialog_->open();
    informationDialog_->raise();
    informationDialog_->activateWindow();
}

/**
 * @brief 슬라이더 오른쪽에 현재 보정 수치를 표시합니다.
 */
void MapSettingsDialog::updatePreprocessingValueLabels() {
    brightnessValueLabel_->setText(
        QStringLiteral("%1%2").arg(brightnessSlider_->value() > 0 ? "+" : "").arg(brightnessSlider_->value()));
    contrastValueLabel_->setText(QString::number(static_cast<double>(contrastSlider_->value()) / 100.0, 'f', 2));
    gammaValueLabel_->setText(QString::number(static_cast<double>(gammaSlider_->value()) / 100.0, 'f', 2));
}

/**
 * @brief         전처리 사용 여부에 맞춰 세부 컨트롤을 활성화합니다.
 * @param enabled 전처리 사용 여부
 */
void MapSettingsDialog::setPreprocessingControlsEnabled(bool enabled) {
    preprocessingPresetComboBox_->setEnabled(enabled);
    brightnessSlider_->setEnabled(enabled);
    contrastSlider_->setEnabled(enabled);
    gammaSlider_->setEnabled(enabled);

    preprocessingControlsWidget_->setProperty("preprocessingActive", enabled);
    preprocessingControlsWidget_->style()->unpolish(preprocessingControlsWidget_);
    preprocessingControlsWidget_->style()->polish(preprocessingControlsWidget_);
    for (QWidget* child : preprocessingControlsWidget_->findChildren<QWidget*>()) {
        child->style()->unpolish(child);
        child->style()->polish(child);
    }
    preprocessingControlsWidget_->update();
}

/**
 * @brief 사용자가 프리셋 값을 직접 변경하면 모드를 사용자 설정으로 전환합니다.
 */
void MapSettingsDialog::markPreprocessingAsCustom() {
    if (updatingPreprocessingControls_) {
        return;
    }
    const QSignalBlocker blocker(preprocessingPresetComboBox_);
    preprocessingPresetComboBox_->setCurrentIndex(customPresetIndex);
}

/**
 * @brief       팝업을 메인 윈도우의 클라이언트 영역 전체에 맞춰 표시합니다.
 * @param event Qt 표시 이벤트
 */
void MapSettingsDialog::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
    }
    raise();
    setFocus(Qt::PopupFocusReason);
}
