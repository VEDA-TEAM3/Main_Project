#pragma once

#include <QLabel>
#include <QPixmap>

class QGraphicsOpacityEffect;
class QPropertyAnimation;

class DangerAlertOverlay final : public QLabel {
    Q_OBJECT

public:
    explicit DangerAlertOverlay(QWidget* parent = nullptr);

    void setActive(bool active);
    void updateGeometryForViewport(const QSize& viewportSize);

private:
    QPixmap sourcePixmap_;
    QGraphicsOpacityEffect* opacityEffect_ = nullptr;
    QPropertyAnimation* opacityAnimation_ = nullptr;
    QPropertyAnimation* fadeOutAnimation_ = nullptr;
    bool active_ = false;
};
