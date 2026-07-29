#include "overlays/DangerAlertOverlay.h"

#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QSize>
#include <QtGlobal>

namespace {
constexpr int alertHorizontalMargin = 18;
constexpr int alertTopMargin = 16;
constexpr int alertMinimumWidth = 170;
constexpr int alertMaximumWidth = 300;
constexpr int alertWidthPercent = 31;
constexpr int opacityAnimationDurationMsec = 1300;
constexpr int fadeOutAnimationDurationMsec = 380;
constexpr qreal alertMaximumOpacity = 0.92;
constexpr qreal alertMinimumOpacity = 0.48;
}  // namespace

/**
 * @brief         맵 viewport 좌상단에 표시할 위험 알림 HUD를 생성합니다.
 * @param parent  QGraphicsView viewport 위젯
 */
DangerAlertOverlay::DangerAlertOverlay(QWidget* parent)
    : QLabel(parent), sourcePixmap_(QStringLiteral(":/icons/danger_alert.png")) {
    setObjectName(QStringLiteral("dangerAlertOverlay"));
    setAlignment(Qt::AlignCenter);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setStyleSheet(QStringLiteral("background: transparent; border: 0;"));

    opacityEffect_ = new QGraphicsOpacityEffect(this);
    opacityEffect_->setOpacity(alertMaximumOpacity);
    setGraphicsEffect(opacityEffect_);

    opacityAnimation_ = new QPropertyAnimation(opacityEffect_, "opacity", this);
    opacityAnimation_->setDuration(opacityAnimationDurationMsec);
    opacityAnimation_->setLoopCount(-1);
    opacityAnimation_->setEasingCurve(QEasingCurve::InOutSine);
    opacityAnimation_->setKeyValueAt(0.0, alertMaximumOpacity);
    opacityAnimation_->setKeyValueAt(0.5, alertMinimumOpacity);
    opacityAnimation_->setKeyValueAt(1.0, alertMaximumOpacity);

    fadeOutAnimation_ = new QPropertyAnimation(opacityEffect_, "opacity", this);
    fadeOutAnimation_->setDuration(fadeOutAnimationDurationMsec);
    fadeOutAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(fadeOutAnimation_, &QPropertyAnimation::finished, this, [this]() {
        if (!active_) {
            hide();
        }
    });

    hide();
}

/**
 * @brief         위험 상태가 유지되는 동안 알림 표시와 점멸을 반복합니다.
 * @param active  하나 이상의 위험 상태가 존재하면 true
 */
void DangerAlertOverlay::setActive(bool active) {
    if (active_ == active) {
        return;
    }

    active_ = active;

    if (!active_) {
        opacityAnimation_->stop();
        fadeOutAnimation_->stop();
        fadeOutAnimation_->setStartValue(opacityEffect_->opacity());
        fadeOutAnimation_->setEndValue(0.0);
        fadeOutAnimation_->start();
        return;
    }

    fadeOutAnimation_->stop();
    opacityEffect_->setOpacity(alertMaximumOpacity);
    show();
    raise();
    opacityAnimation_->start(QAbstractAnimation::KeepWhenStopped);
}

/**
 * @brief               viewport 폭에 비례하되 과도하게 커지지 않도록 알림 크기와 위치를 맞춥니다.
 * @param viewportSize  현재 QGraphicsView viewport 크기
 */
void DangerAlertOverlay::updateGeometryForViewport(const QSize& viewportSize) {
    if (sourcePixmap_.isNull() || viewportSize.isEmpty()) {
        return;
    }

    const int proportionalWidth = viewportSize.width() * alertWidthPercent / 100;
    const int availableWidth = qMax(1, viewportSize.width() - alertHorizontalMargin * 2);
    const int alertWidth = qMin(qBound(alertMinimumWidth, proportionalWidth, alertMaximumWidth), availableWidth);
    const QPixmap scaledPixmap = sourcePixmap_.scaledToWidth(alertWidth, Qt::SmoothTransformation);

    setPixmap(scaledPixmap);
    resize(scaledPixmap.size());
    move(alertHorizontalMargin, alertTopMargin);

    if (active_) {
        raise();
    }
}
