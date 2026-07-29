#pragma once

#include <QWidget>

#include "model/DigitalTwinMapDisplaySettings.h"

class QCheckBox;
class QShowEvent;

class MapSettingsDialog final : public QWidget {
    Q_OBJECT

public:
    explicit MapSettingsDialog(QWidget* parent = nullptr);

    void setSettings(const DigitalTwinMapDisplaySettings& settings);
    void setVideoRiskBordersEnabled(bool enabled);
    void setBlurTargetsEnabled(bool faceEnabled, bool licensePlateEnabled);
    DigitalTwinMapDisplaySettings settings() const;
    bool videoRiskBordersEnabled() const;
    bool faceBlurEnabled() const;
    bool licensePlateBlurEnabled() const;

signals:
    void settingsApplied(const DigitalTwinMapDisplaySettings& settings, bool videoRiskBordersEnabled,
                         bool faceBlurEnabled, bool licensePlateBlurEnabled);

protected:
    void showEvent(QShowEvent* event) override;

private:
    QCheckBox* movementTrailsCheckBox_ = nullptr;
    QCheckBox* ledCheckBox_ = nullptr;
    QCheckBox* cctvCheckBox_ = nullptr;
    QCheckBox* alertDeviceCheckBox_ = nullptr;
    QCheckBox* videoRiskBordersCheckBox_ = nullptr;
    QCheckBox* faceBlurCheckBox_ = nullptr;
    QCheckBox* licensePlateBlurCheckBox_ = nullptr;
};
