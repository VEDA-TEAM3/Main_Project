#pragma once

#include <QColor>
#include <QFrame>
#include <QVariantAnimation>

#include "model/DigitalTwinTypes.h"

class QPaintEvent;

class VideoRiskBorderFrame final : public QFrame {
public:
    explicit VideoRiskBorderFrame(QWidget* parent = nullptr);

    void setRiskLevel(DigitalTwinRiskLevel riskLevel);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor colorForRiskLevel(DigitalTwinRiskLevel riskLevel) const;

    QVariantAnimation borderAnimation_;
    QColor borderColor_ = QColor(0, 0, 0, 0);
    DigitalTwinRiskLevel riskLevel_ = DigitalTwinRiskLevel::Normal;
};
