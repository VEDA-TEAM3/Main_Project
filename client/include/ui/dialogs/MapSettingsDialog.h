#pragma once

#include <QVector>
#include <QWidget>

#include "model/DigitalTwinMapDisplaySettings.h"
#include "model/VideoPreprocessingSettings.h"
#include "video/VideoRuntimeConfig.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QShowEvent;
class QSlider;
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

private:
    void applyPreprocessingPreset(VideoPreprocessingPreset preset);
    void setPreprocessingControls(const VideoPreprocessingSettings& settings);
    void updatePreprocessingValueLabels();
    void setPreprocessingControlsEnabled(bool enabled);
    void markPreprocessingAsCustom();
    void loadPreprocessingChannel(int channelIndex);
    void storeCurrentPreprocessingChannel();
    void rebuildPreprocessingChannelComboBox(int preferredChannelIndex = -1);
    void showPreprocessingAppliedMessage(const QString& message);

    QCheckBox* movementTrailsCheckBox_ = nullptr;
    QCheckBox* ledCheckBox_ = nullptr;
    QCheckBox* cctvCheckBox_ = nullptr;
    QCheckBox* alertDeviceCheckBox_ = nullptr;
    QCheckBox* videoRiskBordersCheckBox_ = nullptr;
    QCheckBox* faceBlurCheckBox_ = nullptr;
    QCheckBox* licensePlateBlurCheckBox_ = nullptr;
    QCheckBox* preprocessingEnabledCheckBox_ = nullptr;
    QComboBox* preprocessingAreaComboBox_ = nullptr;
    QComboBox* preprocessingChannelComboBox_ = nullptr;
    QComboBox* preprocessingPresetComboBox_ = nullptr;
    QSlider* brightnessSlider_ = nullptr;
    QSlider* contrastSlider_ = nullptr;
    QSlider* gammaSlider_ = nullptr;
    QLabel* brightnessValueLabel_ = nullptr;
    QLabel* contrastValueLabel_ = nullptr;
    QLabel* gammaValueLabel_ = nullptr;
    QWidget* preprocessingControlsWidget_ = nullptr;
    InformationDialog* informationDialog_ = nullptr;
    QVector<VideoAreaConfig> videoAreas_;
    QVector<VideoPreprocessingSettings> preprocessingSettingsByChannel_;
    int currentVideoAreaIndex_ = 0;
    int currentPreprocessingChannelIndex_ = 0;
    bool updatingPreprocessingControls_ = false;
};
