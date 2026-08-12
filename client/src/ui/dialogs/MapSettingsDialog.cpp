#include "ui/dialogs/MapSettingsDialog.h"

#include <QQuickItem>
#include <QQuickWidget>
#include <QShowEvent>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>

#include "ui/SharedQmlEngine.h"
#include "ui/dialogs/InformationDialog.h"

namespace {
constexpr int defaultPreprocessingChannelCount = 4;
constexpr int customPresetIndex = 0;
constexpr int dayPresetIndex = 1;
constexpr int nightPresetIndex = 2;
constexpr double preprocessingScale = 100.0;
}  // namespace

/**
 * @brief        UI 표시와 영상 전처리 옵션을 편집하는 모달 팝업을 구성합니다.
 * @param parent 팝업 배경을 덮을 메인 윈도우
 */
MapSettingsDialog::MapSettingsDialog(QWidget* parent) : QWidget(parent) {
    // 자식 위젯으로 메인 윈도우를 덮으면 Qt가 QQuickWidget 텍스처 합성을 건너뛰어 QML 패널이
    // 통째로 사라집니다. 화면 전체를 덮는 딤 없이 패널 크기의 최상위 창으로 띄웁니다.
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setWindowModality(Qt::WindowModal);
    setFocusPolicy(Qt::StrongFocus);

    informationDialog_ = new InformationDialog(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // 최상위 QQuickWidget은 투명 속성을 걸면 아무것도 렌더되지 않으므로 불투명하게 둡니다.
    settingsView_ = new QQuickWidget(sharedQmlEngine(), this);
    settingsView_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    settingsView_->setSource(QUrl(QStringLiteral("qrc:/qml/SettingsDialog.qml")));
    if (!settingsView_->rootObject()) {
        qWarning() << "[MapSettingsDialog] QML load failed" << settingsView_->errors();
        return;
    }
    layout->addWidget(settingsView_);

    QQuickItem* dialogRoot = settingsView_->rootObject();
    setFixedSize(qRound(dialogRoot->implicitWidth()), qRound(dialogRoot->implicitHeight()));
    connectQmlSignals();

    VideoAreaConfig defaultArea;
    defaultArea.areaId = QStringLiteral("default-area");
    defaultArea.name = QStringLiteral("제 1구역");
    for (int localChannelIndex = 0; localChannelIndex < videoChannelsPerArea; ++localChannelIndex) {
        defaultArea.channelIndexes.append(videoGlobalChannelIndex(0, localChannelIndex));
    }
    preprocessingSettingsByChannel_.fill(VideoPreprocessingSettings{}, defaultPreprocessingChannelCount);
    setVideoAreas({defaultArea});
    setVideoPreprocessingSettings(preprocessingSettingsByChannel_);
    hide();
}

/**
 * @brief   QML이 올리는 사용자 조작 신호를 처리기에 연결합니다.
 *
 * @details QML 쪽은 사용자가 직접 만졌을 때만 신호를 올리므로, C++이 값을 되돌려 쓰는
 * 프로그램적 갱신과 구분됩니다.
 */
void MapSettingsDialog::connectQmlSignals() {
    QQuickItem* dialogRoot = settingsView_->rootObject();

    // QML 루트의 신호는 동적 metaobject에만 있어 문자열 기반으로 연결합니다.
    connect(dialogRoot, SIGNAL(cancelled()), this, SLOT(hide()));
    connect(dialogRoot, SIGNAL(applied()), this, SLOT(handleApplied()));
    connect(dialogRoot, SIGNAL(areaSelected(int)), this, SLOT(handleAreaSelected(int)));
    connect(dialogRoot, SIGNAL(channelSelected(int)), this, SLOT(handleChannelSelected(int)));
    connect(dialogRoot, SIGNAL(presetSelected(int)), this, SLOT(handlePresetSelected(int)));
    connect(dialogRoot, SIGNAL(adjusted()), this, SLOT(handleAdjusted()));
    connect(dialogRoot, SIGNAL(preprocessingToggled(bool)), this, SLOT(handlePreprocessingToggled(bool)));
    connect(dialogRoot, SIGNAL(resetRequested()), this, SLOT(handleResetRequested()));
    connect(dialogRoot, SIGNAL(applySelectedRequested()), this, SLOT(handleApplySelectedRequested()));
    connect(dialogRoot, SIGNAL(applyAreaRequested()), this, SLOT(handleApplyAreaRequested()));
    connect(dialogRoot, SIGNAL(applyAllRequested()), this, SLOT(handleApplyAllRequested()));
}

/** @brief 적용 버튼을 눌렀을 때 편집 결과를 알리고 팝업을 닫습니다. */
void MapSettingsDialog::handleApplied() {
    storeCurrentPreprocessingChannel();
    emit settingsApplied(settings(), videoRiskBordersEnabled(), faceBlurEnabled(), licensePlateBlurEnabled(),
                         currentPreprocessingChannelIndex_,
                         preprocessingSettingsByChannel_.value(currentPreprocessingChannelIndex_));
    hide();
}

/**
 * @brief            적용 구역을 바꾸면 해당 구역의 채널 목록으로 다시 채웁니다.
 * @param areaIndex  선택한 구역 인덱스
 */
void MapSettingsDialog::handleAreaSelected(int areaIndex) {
    storeCurrentPreprocessingChannel();
    currentVideoAreaIndex_ = areaIndex;
    rebuildPreprocessingChannelList();
}

/**
 * @brief            적용 대상 채널을 바꾸면 그 채널의 저장값을 불러옵니다.
 * @param listIndex  선택 상자 안에서의 위치
 */
void MapSettingsDialog::handleChannelSelected(int listIndex) {
    storeCurrentPreprocessingChannel();
    const QVector<int> channels = videoAreas_.value(currentVideoAreaIndex_).channelIndexes;
    currentPreprocessingChannelIndex_ = channels.value(listIndex, currentPreprocessingChannelIndex_);
    loadPreprocessingChannel(currentPreprocessingChannelIndex_);
}

/**
 * @brief              주간·야간 모드를 고르면 해당 프리셋 값을 채웁니다.
 * @param presetIndex  선택한 모드 인덱스
 */
void MapSettingsDialog::handlePresetSelected(int presetIndex) {
    if (presetIndex == dayPresetIndex) {
        applyPreprocessingPreset(VideoPreprocessingPreset::Day);
    } else if (presetIndex == nightPresetIndex) {
        applyPreprocessingPreset(VideoPreprocessingPreset::Night);
    }
}

/** @brief 슬라이더를 직접 움직이면 모드를 사용자 설정으로 되돌립니다. */
void MapSettingsDialog::handleAdjusted() { markPreprocessingAsCustom(); }

/**
 * @brief          전처리 사용을 끄면 보정값을 기본값으로 되돌립니다.
 * @param enabled  전처리 사용 여부
 */
void MapSettingsDialog::handlePreprocessingToggled(bool enabled) {
    if (enabled) {
        return;
    }

    VideoPreprocessingSettings settings;
    settings.enabled = false;
    setPreprocessingControls(settings);
}

/** @brief 기본값 복원 버튼을 눌렀을 때 중립 보정값으로 되돌립니다. */
void MapSettingsDialog::handleResetRequested() { applyPreprocessingPreset(VideoPreprocessingPreset::Custom); }

/** @brief 현재 보정값을 선택한 채널 하나에만 적용합니다. */
void MapSettingsDialog::handleApplySelectedRequested() {
    storeCurrentPreprocessingChannel();
    emit videoPreprocessingApplyRequested(currentPreprocessingChannelIndex_,
                                          preprocessingSettingsByChannel_.value(currentPreprocessingChannelIndex_));
    showPreprocessingAppliedMessage(
        QStringLiteral("%1 채널에 변경된 설정이 적용되었습니다!").arg(currentPreprocessingChannelIndex_ + 1));
}

/** @brief 현재 보정값을 선택한 구역의 모든 채널에 적용합니다. */
void MapSettingsDialog::handleApplyAreaRequested() {
    storeCurrentPreprocessingChannel();
    const VideoPreprocessingSettings settings = videoPreprocessingSettings();
    const QVector<int> channels = videoAreas_.value(currentVideoAreaIndex_).channelIndexes;
    for (int channelIndex : channels) {
        if (channelIndex < 0 || channelIndex >= preprocessingSettingsByChannel_.size()) {
            continue;
        }
        preprocessingSettingsByChannel_[channelIndex] = settings;
        emit videoPreprocessingApplyRequested(channelIndex, settings);
    }
    showPreprocessingAppliedMessage(QStringLiteral("%1 전체 채널에 변경된 설정이 적용되었습니다!")
                                        .arg(videoAreas_.value(currentVideoAreaIndex_).name));
}

/** @brief 현재 보정값을 모든 채널에 적용합니다. */
void MapSettingsDialog::handleApplyAllRequested() {
    const VideoPreprocessingSettings settings = videoPreprocessingSettings();
    preprocessingSettingsByChannel_.fill(settings, preprocessingSettingsByChannel_.size());
    emit videoPreprocessingApplyRequested(-1, settings);
    showPreprocessingAppliedMessage(QStringLiteral("전체 채널에 변경된 설정이 적용되었습니다!"));
}

/**
 * @brief       QML 루트 속성 값을 읽습니다.
 * @param name  속성 이름
 * @return      속성 값, 루트가 없으면 invalid
 */
QVariant MapSettingsDialog::qmlValue(const char* name) const {
    if (!settingsView_ || !settingsView_->rootObject()) {
        return {};
    }

    return settingsView_->rootObject()->property(name);
}

/**
 * @brief        QML 루트 속성 값을 설정합니다.
 * @param name   속성 이름
 * @param value  설정할 값
 */
void MapSettingsDialog::setQmlValue(const char* name, const QVariant& value) {
    if (settingsView_ && settingsView_->rootObject()) {
        settingsView_->rootObject()->setProperty(name, value);
    }
}

/**
 * @brief          현재 맵 표시 설정을 체크 항목에 반영합니다.
 * @param settings 편집을 시작할 맵 표시 설정
 */
void MapSettingsDialog::setSettings(const DigitalTwinMapDisplaySettings& settings) {
    setQmlValue("showMovementTrails", settings.showMovementTrails);
    setQmlValue("showLed", settings.showLed);
    setQmlValue("showCctv", settings.showCctv);
    setQmlValue("showAlertDevice", settings.showAlertDevice);
}

/**
 * @brief         CCTV 경고·위험 테두리 알림의 체크 상태를 설정합니다.
 * @param enabled 테두리 알림 표시 여부
 */
void MapSettingsDialog::setVideoRiskBordersEnabled(bool enabled) { setQmlValue("videoRiskBorders", enabled); }

/**
 * @brief                     얼굴·차량 번호판 블러의 체크 상태를 설정합니다.
 * @param faceEnabled         얼굴 블러 활성화 여부
 * @param licensePlateEnabled 차량 번호판 블러 활성화 여부
 */
void MapSettingsDialog::setBlurTargetsEnabled(bool faceEnabled, bool licensePlateEnabled) {
    setQmlValue("faceBlur", faceEnabled);
    setQmlValue("licensePlateBlur", licensePlateEnabled);
}

/**
 * @brief                   영상 설정에서 선택할 CCTV 구역과 채널 구성을 반영합니다.
 * @param areas             구역별 전역 채널 인덱스
 * @param selectedAreaIndex 처음 표시할 구역 인덱스
 */
void MapSettingsDialog::setVideoAreas(const QVector<VideoAreaConfig>& areas, int selectedAreaIndex) {
    if (areas.isEmpty()) {
        return;
    }

    videoAreas_ = areas;
    currentVideoAreaIndex_ = qBound(0, selectedAreaIndex, static_cast<int>(videoAreas_.size()) - 1);

    QStringList areaNames;
    for (const VideoAreaConfig& area : videoAreas_) {
        areaNames.append(area.name);
    }

    setQmlValue("areaNames", areaNames);
    setQmlValue("areaIndex", currentVideoAreaIndex_);
    rebuildPreprocessingChannelList(currentPreprocessingChannelIndex_);
}

/**
 * @brief                      채널별 영상 전처리 설정을 영상 탭에 반영합니다.
 * @param settingsByChannel    채널 인덱스 순서의 영상 전처리 설정
 * @param selectedChannelIndex 처음 표시할 채널 인덱스
 */
void MapSettingsDialog::setVideoPreprocessingSettings(const QVector<VideoPreprocessingSettings>& settingsByChannel,
                                                      int selectedChannelIndex) {
    preprocessingSettingsByChannel_ = settingsByChannel;
    if (preprocessingSettingsByChannel_.isEmpty()) {
        preprocessingSettingsByChannel_.resize(defaultPreprocessingChannelCount);
    }

    const QVector<int> channels = videoAreas_.value(currentVideoAreaIndex_).channelIndexes;
    currentPreprocessingChannelIndex_ =
        channels.contains(selectedChannelIndex) ? selectedChannelIndex : channels.value(0, 0);
    rebuildPreprocessingChannelList(currentPreprocessingChannelIndex_);
    loadPreprocessingChannel(currentPreprocessingChannelIndex_);
}

/**
 * @brief          영상 전처리 설정 하나를 현재 컨트롤에 반영합니다.
 * @param settings 표시할 영상 전처리 설정
 */
void MapSettingsDialog::setPreprocessingControls(const VideoPreprocessingSettings& settings) {
    updatingPreprocessingControls_ = true;
    setQmlValue("preprocessingEnabled", settings.enabled);
    setQmlValue("brightness", settings.brightness);
    setQmlValue("contrast", qRound(settings.contrast * preprocessingScale));
    setQmlValue("gamma", qRound(settings.gamma * preprocessingScale));

    int presetIndex = customPresetIndex;
    if (settings.preset == VideoPreprocessingPreset::Day) {
        presetIndex = dayPresetIndex;
    } else if (settings.preset == VideoPreprocessingPreset::Night) {
        presetIndex = nightPresetIndex;
    }
    setQmlValue("presetIndex", presetIndex);
    updatingPreprocessingControls_ = false;
}

/**
 * @brief  사용자가 선택한 네 개 표시 옵션을 반환합니다.
 * @return 현재 체크 상태로 구성한 맵 표시 설정
 */
DigitalTwinMapDisplaySettings MapSettingsDialog::settings() const {
    DigitalTwinMapDisplaySettings displaySettings;
    displaySettings.showMovementTrails = qmlValue("showMovementTrails").toBool();
    displaySettings.showLed = qmlValue("showLed").toBool();
    displaySettings.showCctv = qmlValue("showCctv").toBool();
    displaySettings.showAlertDevice = qmlValue("showAlertDevice").toBool();
    return displaySettings;
}

/**
 * @brief  CCTV 테두리 알림 표시 여부를 반환합니다.
 * @return 테두리 알림을 표시하면 true
 */
bool MapSettingsDialog::videoRiskBordersEnabled() const { return qmlValue("videoRiskBorders").toBool(); }

/**
 * @brief  얼굴 블러 표시 여부를 반환합니다.
 * @return 얼굴 블러가 활성화되어 있으면 true
 */
bool MapSettingsDialog::faceBlurEnabled() const { return qmlValue("faceBlur").toBool(); }

/**
 * @brief  차량 번호판 블러 표시 여부를 반환합니다.
 * @return 차량 번호판 블러가 활성화되어 있으면 true
 */
bool MapSettingsDialog::licensePlateBlurEnabled() const { return qmlValue("licensePlateBlur").toBool(); }

/**
 * @brief  현재 영상 탭 값을 전처리 설정 모델로 변환합니다.
 * @return 모든 채널 수신기에 전달할 영상 전처리 설정
 */
VideoPreprocessingSettings MapSettingsDialog::videoPreprocessingSettings() const {
    VideoPreprocessingSettings settings;
    settings.enabled = qmlValue("preprocessingEnabled").toBool();
    if (!settings.enabled) {
        return settings;
    }

    settings.brightness = qmlValue("brightness").toInt();
    settings.contrast = qmlValue("contrast").toDouble() / preprocessingScale;
    settings.gamma = qmlValue("gamma").toDouble() / preprocessingScale;

    const int presetIndex = qmlValue("presetIndex").toInt();
    if (presetIndex == dayPresetIndex) {
        settings.preset = VideoPreprocessingPreset::Day;
    } else if (presetIndex == nightPresetIndex) {
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
    settings.enabled = preset == VideoPreprocessingPreset::Custom || qmlValue("preprocessingEnabled").toBool();
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
 * @brief                       현재 구역에 속한 채널만 적용 대상 선택 상자에 표시합니다.
 * @param preferredChannelIndex 유지할 전역 채널 인덱스, 없으면 구역 첫 채널
 */
void MapSettingsDialog::rebuildPreprocessingChannelList(int preferredChannelIndex) {
    const QVector<int> channels = videoAreas_.value(currentVideoAreaIndex_).channelIndexes;
    if (channels.isEmpty()) {
        return;
    }

    QStringList channelNames;
    for (int channelIndex : channels) {
        channelNames.append(
            QStringLiteral("CH %1").arg(videoLocalChannelNumber(channelIndex), 2, 10, QLatin1Char('0')));
    }

    const qsizetype foundIndex = channels.indexOf(preferredChannelIndex);
    const int listIndex = foundIndex < 0 ? 0 : static_cast<int>(foundIndex);

    setQmlValue("channelNames", channelNames);
    setQmlValue("channelIndex", listIndex);
    currentPreprocessingChannelIndex_ = channels[listIndex];
    loadPreprocessingChannel(currentPreprocessingChannelIndex_);
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
 * @brief 사용자가 프리셋 값을 직접 변경하면 모드를 사용자 설정으로 전환합니다.
 */
void MapSettingsDialog::markPreprocessingAsCustom() {
    if (updatingPreprocessingControls_) {
        return;
    }
    setQmlValue("presetIndex", customPresetIndex);
}

/**
 * @brief       팝업을 메인 윈도우 중앙에 맞춰 표시합니다.
 * @param event Qt 표시 이벤트
 */
void MapSettingsDialog::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (parentWidget()) {
        const QPoint parentCenter = parentWidget()->mapToGlobal(parentWidget()->rect().center());
        move(parentCenter - QPoint(width() / 2, height() / 2));
    }
    raise();
    setFocus(Qt::PopupFocusReason);
}
