#include "ui/dialogs/MapSettingsDialog.h"

#include <QCheckBox>
#include <QColor>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QPushButton>
#include <QShowEvent>
#include <QStyle>
#include <QStyleOptionButton>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr int dialogPanelWidth = 720;
constexpr int dialogPanelHeight = 600;

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

QCheckBox* createOptionCheckBox(const QString& text, QWidget* parent) {
    auto* checkBox = new MapOptionCheckBox(text, parent);
    checkBox->setObjectName(QStringLiteral("mapSettingsOption"));
    checkBox->setCursor(Qt::PointingHandCursor);
    return checkBox;
}
}  // namespace

/**
 * @brief        디지털 트윈 맵 표시 옵션을 편집하는 모달 팝업을 구성합니다.
 * @param parent 팝업 배경을 덮을 메인 윈도우
 */
MapSettingsDialog::MapSettingsDialog(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("mapSettingsDialog"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);

    auto* overlayLayout = new QVBoxLayout(this);
    overlayLayout->setContentsMargins(0, 0, 0, 0);
    overlayLayout->setAlignment(Qt::AlignCenter);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("mapSettingsDialogPanel"));
    panel->setFixedSize(dialogPanelWidth, dialogPanelHeight);
    overlayLayout->addWidget(panel);

    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(32, 26, 32, 28);
    panelLayout->setSpacing(22);

    auto* headerLayout = new QHBoxLayout();
    auto* titleLabel = new QLabel(QStringLiteral("설정"), panel);
    titleLabel->setObjectName(QStringLiteral("mapSettingsTitleLabel"));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch(1);

    auto* closeButton = new QPushButton(panel);
    closeButton->setObjectName(QStringLiteral("mapSettingsCloseButton"));
    closeButton->setText(QStringLiteral("×"));
    closeButton->setToolTip(QStringLiteral("닫기"));
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, this, &QWidget::hide);
    headerLayout->addWidget(closeButton);
    panelLayout->addLayout(headerLayout);

    auto* optionsFrame = new QFrame(panel);
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

    auto* cctvSectionTitleLabel = new QLabel(QStringLiteral("CCTV 알림 설정"), optionsFrame);
    cctvSectionTitleLabel->setObjectName(QStringLiteral("mapSettingsSectionLabel"));
    optionsLayout->addWidget(cctvSectionTitleLabel);

    videoRiskBordersCheckBox_ = createOptionCheckBox(QStringLiteral("CCTV 테두리 알림 표시"), optionsFrame);
    optionsLayout->addWidget(videoRiskBordersCheckBox_);

    auto* blurSectionTitleLabel = new QLabel(QStringLiteral("블러 설정"), optionsFrame);
    blurSectionTitleLabel->setObjectName(QStringLiteral("mapSettingsSectionLabel"));
    optionsLayout->addWidget(blurSectionTitleLabel);

    auto* blurOptionGrid = new QGridLayout();
    blurOptionGrid->setHorizontalSpacing(72);
    blurOptionGrid->setColumnStretch(0, 1);
    blurOptionGrid->setColumnStretch(1, 1);
    faceBlurCheckBox_ = createOptionCheckBox(QStringLiteral("얼굴"), optionsFrame);
    licensePlateBlurCheckBox_ = createOptionCheckBox(QStringLiteral("차량 번호판"), optionsFrame);
    blurOptionGrid->addWidget(faceBlurCheckBox_, 0, 0);
    blurOptionGrid->addWidget(licensePlateBlurCheckBox_, 0, 1);
    optionsLayout->addLayout(blurOptionGrid);
    panelLayout->addWidget(optionsFrame, 1);

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
        emit settingsApplied(settings(), videoRiskBordersEnabled(), faceBlurEnabled(), licensePlateBlurEnabled());
        hide();
    });
    buttonLayout->addWidget(applyButton);
    panelLayout->addLayout(buttonLayout);

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
 * @brief  사용자가 선택한 CCTV 테두리 알림 표시 여부를 반환합니다.
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
