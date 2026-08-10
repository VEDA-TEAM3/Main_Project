#pragma once

#include <QColor>
#include <QComboBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QPolygonF>

class DashboardAreaComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent* event) override {
        QComboBox::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor arrowColor = isEnabled() ? QColor(QStringLiteral("#8edfff")) : QColor(QStringLiteral("#4a6b7d"));
        painter.setPen(QPen(arrowColor, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

        const qreal centerX = width() - 17.0;
        const qreal centerY = height() / 2.0;
        QPolygonF chevron;
        chevron << QPointF(centerX - 4.0, centerY - 2.0) << QPointF(centerX, centerY + 2.0)
                << QPointF(centerX + 4.0, centerY - 2.0);
        painter.drawPolyline(chevron);
    }
};
