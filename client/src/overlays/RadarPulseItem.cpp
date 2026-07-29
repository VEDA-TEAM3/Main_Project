#include "overlays/RadarPulseItem.h"

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QString>
#include <QtGlobal>
#include <algorithm>

namespace {
constexpr qreal minimumRadius = 10.0;
constexpr qreal warningMaximumRadius = 142.0;
constexpr qreal dangerMaximumRadius = 180.0;
constexpr qreal boundingPadding = 20.0;
constexpr qreal warningRingDelay = 0.18;
constexpr qreal dangerRingDelay = 0.16;
constexpr int warningAnimationDurationMsec = 1400;
constexpr int dangerAnimationDurationMsec = 1800;

/**
 * @brief        애니메이션 진행률을 0.0~1.0 범위로 제한합니다.
 * @param value  제한 전 진행률
 * @return       제한된 진행률
 */
qreal clampedProgress(qreal value) { return std::clamp(value, static_cast<qreal>(0.0), static_cast<qreal>(1.0)); }
}  // namespace

/**
 * @brief            감지 단계에 맞는 레이더 펄스 항목을 생성합니다.
 * @param riskLevel  표시할 주의 또는 위험 단계
 * @param parent     부모 그래픽 항목
 */
RadarPulseItem::RadarPulseItem(DigitalTwinRiskLevel riskLevel, QGraphicsItem* parent)
    : QGraphicsItem(parent), riskLevel_(riskLevel) {
    setAcceptedMouseButtons(Qt::NoButton);
    setZValue(3.4);
}

/**
 * @brief 펄스가 그려질 수 있는 최대 영역을 반환합니다.
 * @return 펄스 경계 영역
 */
QRectF RadarPulseItem::boundingRect() const {
    const qreal radius = maximumRadius() + boundingPadding;
    return QRectF(-radius, -radius, radius * 2.0, radius * 2.0);
}

/**
 * @brief         현재 진행률에 맞춰 확산 원과 중심광을 그립니다.
 * @param painter  그래픽 항목을 그릴 QPainter
 * @param option   그래픽 항목 스타일 옵션
 * @param widget   렌더링 대상 위젯
 */
void RadarPulseItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    Q_UNUSED(option)
    Q_UNUSED(widget)

    if (!painter) {
        return;
    }

    painter->setRenderHint(QPainter::Antialiasing, true);

    const QColor baseColor = pulseColor();
    const bool danger = riskLevel_ == DigitalTwinRiskLevel::Danger;
    const int totalRingCount = ringCount();
    const qreal maxRadius = maximumRadius();

    for (int ringIndex = 0; ringIndex < totalRingCount; ++ringIndex) {
        const qreal phase = ringPhase(ringIndex);

        if (phase <= 0.0) {
            continue;
        }

        const qreal radius = minimumRadius + (maxRadius - minimumRadius) * phase;
        const qreal remaining = 1.0 - phase;
        const qreal fade = remaining * remaining * (3.0 - 2.0 * remaining);
        const int alpha = static_cast<int>((danger ? 255.0 : 220.0) * fade);

        QColor outerGlowColor = baseColor;
        outerGlowColor.setAlpha(std::max(0, danger ? alpha / 5 : alpha * 4 / 25));
        QPen outerGlowPen(outerGlowColor, danger ? 19.0 : 15.0);
        outerGlowPen.setCosmetic(true);
        painter->setPen(outerGlowPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(QPointF(0.0, 0.0), radius, radius);

        QColor innerGlowColor = baseColor;
        innerGlowColor.setAlpha(std::max(0, danger ? alpha * 2 / 5 : alpha * 8 / 25));
        QPen innerGlowPen(innerGlowColor, danger ? 8.5 : 7.0);
        innerGlowPen.setCosmetic(true);
        painter->setPen(innerGlowPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(QPointF(0.0, 0.0), radius, radius);

        QColor lineColor = baseColor;
        lineColor.setAlpha(std::max(0, danger ? alpha * 9 / 10 : alpha * 4 / 5));
        QPen linePen(lineColor, danger ? 2.6 : 2.0);
        linePen.setCosmetic(true);
        painter->setPen(linePen);
        painter->drawEllipse(QPointF(0.0, 0.0), radius, radius);
    }

    const qreal centerDuration = danger ? 0.42 : 0.4;
    if (progress_ < centerDuration) {
        const qreal centerFade = 1.0 - progress_ / centerDuration;
        const qreal centerRadius = 8.0 + progress_ * (danger ? 30.0 : 24.0);

        QColor centerGlowColor = baseColor;
        centerGlowColor.setAlpha(static_cast<int>((danger ? 58.0 : 34.0) * centerFade));
        painter->setPen(Qt::NoPen);
        painter->setBrush(centerGlowColor);
        painter->drawEllipse(QPointF(0.0, 0.0), centerRadius * 1.7, centerRadius * 1.7);

        QColor centerColor = baseColor;
        centerColor.setAlpha(static_cast<int>((danger ? 150.0 : 105.0) * centerFade * centerFade));
        painter->setBrush(centerColor);
        painter->drawEllipse(QPointF(0.0, 0.0), centerRadius, centerRadius);
    }
}

/**
 * @brief 현재 애니메이션 진행률을 반환합니다.
 * @return 0.0~1.0 범위의 진행률
 */
qreal RadarPulseItem::progress() const { return progress_; }

/**
 * @brief             애니메이션 진행률을 갱신합니다.
 * @param progress    0.0~1.0 범위로 제한할 진행률
 */
void RadarPulseItem::setProgress(qreal progress) {
    progress_ = clampedProgress(progress);
    update();
}

/**
 * @brief              경과 시간을 기준으로 진행률을 갱신합니다.
 * @param elapsedMsec  펄스 시작 후 경과 시간
 */
void RadarPulseItem::setElapsedMsec(int elapsedMsec) {
    const int duration = durationMsec();

    if (duration <= 0) {
        setProgress(1.0);
        return;
    }

    setProgress(static_cast<qreal>(elapsedMsec) / static_cast<qreal>(duration));
}

/**
 * @brief 펄스 애니메이션의 종료 여부를 반환합니다.
 * @return 진행률이 끝에 도달하면 true
 */
bool RadarPulseItem::isFinished() const { return progress_ >= 1.0; }

/**
 * @brief 위험 단계에 맞는 펄스 색상을 반환합니다.
 * @return 주의 또는 위험 색상
 */
QColor RadarPulseItem::pulseColor() const {
    if (riskLevel_ == DigitalTwinRiskLevel::Danger) {
        return QColor(QStringLiteral("#ff2f3d"));
    }

    return QColor(QStringLiteral("#ffd43b"));
}

/**
 * @brief 위험 단계에 맞는 원 개수를 반환합니다.
 * @return 표시할 확산 원 개수
 */
int RadarPulseItem::ringCount() const { return riskLevel_ == DigitalTwinRiskLevel::Danger ? 4 : 3; }

/**
 * @brief            지정한 원의 지연 시간을 반영한 진행률을 계산합니다.
 * @param ringIndex  계산할 원 인덱스
 * @return           해당 원의 진행률
 */
qreal RadarPulseItem::ringPhase(int ringIndex) const {
    const qreal delay = riskLevel_ == DigitalTwinRiskLevel::Danger ? dangerRingDelay : warningRingDelay;
    const qreal totalDelay = static_cast<qreal>(ringCount() - 1) * delay;
    const qreal normalizedTime = progress_ * (1.0 + totalDelay);
    return clampedProgress(normalizedTime - static_cast<qreal>(ringIndex) * delay);
}

/**
 * @brief 위험 단계에 맞는 애니메이션 시간을 반환합니다.
 * @return 애니메이션 시간
 */
int RadarPulseItem::durationMsec() const {
    return riskLevel_ == DigitalTwinRiskLevel::Danger ? dangerAnimationDurationMsec : warningAnimationDurationMsec;
}

/**
 * @brief 위험 단계에 맞는 최대 반지름을 반환합니다.
 * @return 최대 펄스 반지름
 */
qreal RadarPulseItem::maximumRadius() const {
    return riskLevel_ == DigitalTwinRiskLevel::Danger ? dangerMaximumRadius : warningMaximumRadius;
}
