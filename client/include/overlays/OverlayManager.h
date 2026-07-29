#pragma once

#include <QGraphicsScene>
#include <QPointF>
#include <QTimer>
#include <QVector>
#include <memory>

#include "model/DigitalTwinTypes.h"

class RadarPulseItem;

class OverlayManager final {
public:
    OverlayManager();
    ~OverlayManager();

    void setScene(QGraphicsScene* scene);
    void showRiskPulse(const QPointF& scenePosition, DigitalTwinRiskLevel riskLevel);
    void clear();

private:
    void updateAnimations();
    void removePulseAt(qsizetype index);

    struct ActivePulseItem {
        std::shared_ptr<RadarPulseItem> item;
        int elapsedMsec = 0;
    };

    QGraphicsScene* scene_ = nullptr;
    QTimer animationTimer_;
    QVector<ActivePulseItem> activePulseItems_;
};
