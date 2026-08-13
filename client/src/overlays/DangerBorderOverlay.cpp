#include "overlays/DangerBorderOverlay.h"

#include <QBrush>
#include <QColor>
#include <QObject>
#include <QPen>
#include <QtGlobal>

namespace {
constexpr int overlayFrameIntervalMsec = 33;
/// 한 번 밝아졌다 어두워지는 데 걸리는 시간
constexpr qint64 blinkPeriodMsec = 1300;
/// 짧게 스치는 위험도 알아볼 수 있도록 최소한 이만큼은 깜박인다
constexpr qint64 minimumVisibleMsec = 2000;
/// 위험이 풀린 뒤 사라지는 데 걸리는 시간
constexpr qreal fadeOutStepPerFrame = static_cast<qreal>(overlayFrameIntervalMsec) / 380.0;
constexpr qreal minimumOpacity = 0.35;
constexpr qreal maximumOpacity = 1.0;
constexpr qreal borderPenWidth = 3.0;
/// 객체(4.0)와 상태 칩(5.2)보다 위에 그린다
constexpr qreal borderZValue = 20.0;
}  // namespace

/** @brief 깜박임 갱신 타이머를 준비합니다. 타이머는 이 객체를 만든 UI 스레드에서 돕니다. */
DangerBorderOverlay::DangerBorderOverlay() {
    animationTimer_.setInterval(overlayFrameIntervalMsec);
    animationTimer_.setTimerType(Qt::PreciseTimer);

    QObject::connect(&animationTimer_, &QTimer::timeout, [this]() { updateAnimation(); });
}

DangerBorderOverlay::~DangerBorderOverlay() { clear(); }

/**
 * @brief             지도 테두리 사각형을 scene에 올립니다.
 * @param scene       테두리를 올릴 scene
 * @param borderRect  지도 전체를 감싸는 사각형
 */
void DangerBorderOverlay::attach(QGraphicsScene* scene, const QRectF& borderRect) {
    clear();

    if (!scene || borderRect.isEmpty()) {
        return;
    }

    QPen borderPen(QColor(QStringLiteral("#ff2f3d")), borderPenWidth);
    borderPen.setJoinStyle(Qt::MiterJoin);

    item_ = std::make_unique<QGraphicsRectItem>(borderRect);
    item_->setPen(borderPen);
    item_->setBrush(QBrush(Qt::NoBrush));
    item_->setZValue(borderZValue);
    item_->setOpacity(0.0);
    item_->setVisible(false);
    // 구역 클릭은 뷰가 좌표로 판정하므로 이 아이템이 입력을 가로채면 안 된다
    item_->setAcceptedMouseButtons(Qt::NoButton);
    scene->addItem(item_.get());
}

/**
 * @brief         위험 상태 유무를 반영합니다.
 * @param active  하나 이상의 위험 상태가 존재하면 true
 */
void DangerBorderOverlay::setActive(bool active) {
    if (active_ == active) {
        return;
    }

    active_ = active;
    if (active_) {
        elapsedMsec_ = 0;
    }

    if (item_ && !animationTimer_.isActive()) {
        animationTimer_.start();
    }
}

/** @brief scene에 올린 테두리를 떼어 냅니다. */
void DangerBorderOverlay::clear() {
    animationTimer_.stop();

    if (item_ && item_->scene()) {
        item_->scene()->removeItem(item_.get());
    }

    item_.reset();
    elapsedMsec_ = 0;
    opacity_ = 0.0;
    active_ = false;
}

/** @brief 테두리 밝기를 한 프레임만큼 갱신합니다. */
void DangerBorderOverlay::updateAnimation() {
    if (!item_) {
        animationTimer_.stop();
        return;
    }

    elapsedMsec_ += overlayFrameIntervalMsec;

    // 위험이 풀려도 최소 표시 시간까지는 계속 깜박인다
    const bool blinking = active_ || elapsedMsec_ < minimumVisibleMsec;
    if (blinking) {
        // 삼각파를 smoothstep으로 눕혀 사인처럼 부드럽게 밝아졌다 어두워지게 한다
        const double phase = static_cast<double>(elapsedMsec_ % blinkPeriodMsec) / blinkPeriodMsec;
        const double triangle = phase < 0.5 ? 1.0 - phase * 2.0 : (phase - 0.5) * 2.0;
        const double eased = triangle * triangle * (3.0 - 2.0 * triangle);
        opacity_ = minimumOpacity + (maximumOpacity - minimumOpacity) * eased;
    } else {
        opacity_ = qMax(0.0, opacity_ - fadeOutStepPerFrame);
    }

    item_->setOpacity(opacity_);
    item_->setVisible(opacity_ > 0.0);

    if (!blinking && opacity_ <= 0.0) {
        animationTimer_.stop();
    }
}
