#include "ui/VideoRiskBorderFrame.h"

#include <QEasingCurve>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QString>
#include <QVariant>

namespace {
constexpr int fadeInDurationMsec = 400;
constexpr int fadeOutDurationMsec = 530;
constexpr qreal borderWidth = 3.0;
constexpr qreal borderRadius = 5.0;
}  // namespace

/**
 * @brief         영상 타일 위에 독립적으로 애니메이션되는 위험 테두리를 구성합니다.
 * @param parent  부모 CCTV 카드 위젯
 */
VideoRiskBorderFrame::VideoRiskBorderFrame(QWidget* parent) : QFrame(parent) {
    borderAnimation_.setEasingCurve(QEasingCurve::InOutQuad);
    connect(&borderAnimation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        borderColor_ = value.value<QColor>();
        update();
    });
}

/**
 * @brief            현재 채널 위험 단계로 테두리 색상을 부드럽게 전환합니다.
 * @param riskLevel  새 위험 단계
 */
void VideoRiskBorderFrame::setRiskLevel(DigitalTwinRiskLevel riskLevel) {
    if (riskLevel_ == riskLevel) {
        return;
    }

    riskLevel_ = riskLevel;
    borderAnimation_.stop();
    borderAnimation_.setDuration(riskLevel == DigitalTwinRiskLevel::Normal ? fadeOutDurationMsec : fadeInDurationMsec);
    borderAnimation_.setStartValue(borderColor_);
    borderAnimation_.setEndValue(colorForRiskLevel(riskLevel));
    borderAnimation_.start();
}

/**
 * @brief        기본 프레임 위에 현재 애니메이션 색상의 둥근 테두리를 그립니다.
 * @param event  Qt 페인트 이벤트
 */
void VideoRiskBorderFrame::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);

    if (borderColor_.alpha() <= 0) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPen borderPen(borderColor_, borderWidth);
    borderPen.setCosmetic(true);
    painter.setPen(borderPen);
    painter.setBrush(Qt::NoBrush);

    const qreal inset = borderWidth * 0.5;
    const QRectF borderRect = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    painter.drawRoundedRect(borderRect, borderRadius, borderRadius);
}

/**
 * @brief            이벤트 로그와 동일한 위험 단계 색상을 반환합니다.
 * @param riskLevel  변환할 위험 단계
 * @return           투명, 경고 또는 위험 테두리 색상
 */
QColor VideoRiskBorderFrame::colorForRiskLevel(DigitalTwinRiskLevel riskLevel) const {
    switch (riskLevel) {
        case DigitalTwinRiskLevel::Warning:
            return QColor(QStringLiteral("#ffd43b"));
        case DigitalTwinRiskLevel::Danger:
            return QColor(QStringLiteral("#ff2f3d"));
        case DigitalTwinRiskLevel::Normal:
            return QColor(0, 0, 0, 0);
    }

    return QColor(0, 0, 0, 0);
}
