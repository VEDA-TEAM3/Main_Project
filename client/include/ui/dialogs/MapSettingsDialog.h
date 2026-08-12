#pragma once

#include <QVariant>
#include <QVector>
#include <QWidget>

#include "model/DigitalTwinMapDisplaySettings.h"
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

private:
    void connectQmlSignals();
    QVariant qmlValue(const char* name) const;
    void setQmlValue(const char* name, const QVariant& value);
    void applyPreprocessingPreset(VideoPreprocessingPreset preset);
    void setPreprocessingControls(const VideoPreprocessingSettings& settings);
    void markPreprocessingAsCustom();
    void loadPreprocessingChannel(int channelIndex);
    void storeCurrentPreprocessingChannel();
    void rebuildPreprocessingChannelList(int preferredChannelIndex = -1);
    void showPreprocessingAppliedMessage(const QString& message);

    QQuickWidget* settingsView_ = nullptr;
    InformationDialog* informationDialog_ = nullptr;
    QVector<VideoAreaConfig> videoAreas_;
    QVector<VideoPreprocessingSettings> preprocessingSettingsByChannel_;
    int currentVideoAreaIndex_ = 0;
    int currentPreprocessingChannelIndex_ = 0;
    bool updatingPreprocessingControls_ = false;
};
