#pragma once

#include <QColor>
#include <QGraphicsItem>

#include "model/DigitalTwinTypes.h"

class QPainter;
class QStyleOptionGraphicsItem;
class QWidget;

class RadarPulseItem : public QGraphicsItem {
public:
    explicit RadarPulseItem(DigitalTwinRiskLevel riskLevel, QGraphicsItem* parent = nullptr);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

    qreal progress() const;
    void setProgress(qreal progress);
    void setElapsedMsec(int elapsedMsec);
    bool isFinished() const;

private:
    qreal ringPhase(int ringIndex) const;
    QColor pulseColor() const;
    int ringCount() const;
    int durationMsec() const;
    qreal maximumRadius() const;

    DigitalTwinRiskLevel riskLevel_ = DigitalTwinRiskLevel::Normal;
    qreal progress_ = 0.0;
};
