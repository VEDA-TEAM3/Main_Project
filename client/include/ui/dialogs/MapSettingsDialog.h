#pragma once

#include <QStringList>
#include <QVariant>
#include <QVector>
#include <QWidget>

#include "model/DigitalTwinMapDisplaySettings.h"
#include "model/StreamConfig.h"
#include "model/VideoPreprocessingSettings.h"
#include "video/VideoRuntimeConfig.h"

class QQuickWidget;
class QShowEvent;
class InformationDialog;

class MapSettingsDialog final : public QWidget {
    Q_OBJECT

public:
    explicit MapSettingsDialog(QWidget* parent = nullptr);

    void setSettings(const DigitalTwinMapDisplaySettings& settings);
    void setVideoRiskBordersEnabled(bool enabled);
    void setBlurTargetsEnabled(bool faceEnabled, bool licensePlateEnabled);
    void setVideoAreas(const QVector<VideoAreaConfig>& areas, int selectedAreaIndex = 0);
    void setConfigSourcePath(const QString& configSourcePath);
    void setStreamConfigs(const QVector<StreamConfig>& streams);
    void setVideoPreprocessingSettings(const QVector<VideoPreprocessingSettings>& settingsByChannel,
                                       int selectedChannelIndex = 0);
    DigitalTwinMapDisplaySettings settings() const;
    bool videoRiskBordersEnabled() const;
    bool faceBlurEnabled() const;
    bool licensePlateBlurEnabled() const;
    VideoPreprocessingSettings videoPreprocessingSettings() const;

signals:
    void settingsApplied(const DigitalTwinMapDisplaySettings& settings, bool videoRiskBordersEnabled,
                         bool faceBlurEnabled, bool licensePlateBlurEnabled, int preprocessingChannelIndex,
                         const VideoPreprocessingSettings& preprocessingSettings);
    void videoPreprocessingApplyRequested(int channelIndex, const VideoPreprocessingSettings& settings);
    /** @brief 구역 목록에서 고른 구역으로 화면을 전환하도록 요청합니다. */
    void videoAreaSelected(int areaIndex);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void handleApplied();
    void handleAreaSelected(int areaIndex);
    void handleChannelSelected(int listIndex);
    void handlePresetSelected(int presetIndex);
    void handleAdjusted();
    void handlePreprocessingToggled(bool enabled);
    void handleResetRequested();
    void handleApplySelectedRequested();
    void handleApplyAreaRequested();
    void handleApplyAllRequested();
    void handleZoneAddRequested();
    void handleZoneSaveRequested(int zoneIndex);
    void handleZoneSelectRequested(int zoneIndex);

private:
    void connectQmlSignals();
    void refreshZoneTab();
    void loadZoneIntoForm(int zoneIndex);
    QStringList zoneFormAddresses() const;
    void setZoneMessage(const QString& message, bool isError);
    QVariant qmlValue(const char* name) const;
    void setQmlValue(const char* name, const QVariant& value);
    void applyPreprocessingPreset(VideoPreprocessingPreset preset);
    void setPreprocessingControls(const VideoPreprocessingSettings& settings);
    void markPreprocessingAsCustom();
    void loadPreprocessingChannel(int channelIndex);
    void storeCurrentPreprocessingChannel();
    void rebuildPreprocessingChannelList(int preferredChannelIndex = -1);
    void showInformation(const QString& title, const QString& message);

    QQuickWidget* settingsView_ = nullptr;
    InformationDialog* informationDialog_ = nullptr;
    QString configSourcePath_;
    /// 설정 파일에는 저장됐지만 아직 실행에 반영되지 않은 구역 이름
    QStringList pendingZoneNames_;
    /// 값을 고쳐 저장했지만 아직 실행에 반영되지 않은 구역 번호
    QVariantList editedZoneRows_;
    QVector<StreamConfig> streamConfigs_;
    QVector<VideoAreaConfig> videoAreas_;
    /// 지금 화면에 띄워 둔 구역. 영상 탭의 "적용 구역"과는 별개다
    int displayedZoneIndex_ = 0;
    QVector<VideoPreprocessingSettings> preprocessingSettingsByChannel_;
    int currentVideoAreaIndex_ = 0;
    int currentPreprocessingChannelIndex_ = 0;
    bool updatingPreprocessingControls_ = false;
};
