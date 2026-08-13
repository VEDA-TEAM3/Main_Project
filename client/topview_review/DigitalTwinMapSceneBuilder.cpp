#include "ui/DigitalTwinMapSceneBuilder.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QGraphicsScene>
#include <QLineF>
#include <QPen>
#include <QString>

namespace {
constexpr double sceneWidth = 1000.0;
constexpr double sceneHeight = 520.0;
constexpr double zoneSize = 420.0;
constexpr double zoneTop = 50.0;
constexpr double leftZoneLeft = 80.0;
constexpr double rightZoneLeft = leftZoneLeft + zoneSize;
constexpr int channelsPerZone = 4;

void addCenteredLabel(QGraphicsScene* scene, const QString& text, const QFont& font, const QColor& color,
                      double opacity, const QPointF& center) {
    auto* label = scene->addSimpleText(text, font);
    label->setBrush(color);
    label->setOpacity(opacity);
    label->setZValue(1.5);
    const QRectF bounds = label->boundingRect();
    label->setPos(center.x() - bounds.width() * 0.5, center.y() - bounds.height() * 0.5);
}

void addChannelLabels(QGraphicsScene* scene, const QRectF& zoneRect) {
    QFont channelFont(QStringLiteral("Segoe UI"));
    channelFont.setPointSizeF(15.0);
    channelFont.setWeight(QFont::DemiBold);

    const std::array<QPointF, channelsPerZone> centers = {
        QPointF(zoneRect.center().x(), zoneRect.top() + zoneRect.height() * 0.27),
        QPointF(zoneRect.right() - zoneRect.width() * 0.27, zoneRect.center().y()),
        QPointF(zoneRect.center().x(), zoneRect.bottom() - zoneRect.height() * 0.27),
        QPointF(zoneRect.left() + zoneRect.width() * 0.27, zoneRect.center().y()),
    };

    for (int localChannelIndex = 0; localChannelIndex < channelsPerZone; ++localChannelIndex) {
        const int displayChannelNumber = localChannelIndex + 1;
        addCenteredLabel(scene, QStringLiteral("CH%1").arg(displayChannelNumber, 2, 10, QLatin1Char('0')), channelFont,
                         QColor(QStringLiteral("#6f9ab8")), 0.52, centers[localChannelIndex]);
    }
}

void addZone(QGraphicsScene* scene, const QRectF& zoneRect, int zoneNumber) {
    const QPen wallPen(QColor(QStringLiteral("#3a4c63")), 2.6);
    const QPen guidePen(QColor(QStringLiteral("#31475d")), 1.1);
    const QPen slotPen(QColor(QStringLiteral("#2d435b")), 1.3);

    scene->addRect(zoneRect, wallPen, QBrush(QColor(QStringLiteral("#0a1523"))))->setZValue(1.0);
    scene->addLine(QLineF(zoneRect.topLeft(), zoneRect.bottomRight()), guidePen)->setZValue(1.2);
    scene->addLine(QLineF(zoneRect.topRight(), zoneRect.bottomLeft()), guidePen)->setZValue(1.2);

    const double roadTop = zoneRect.top() + zoneRect.height() * 0.34;
    const double roadBottom = zoneRect.top() + zoneRect.height() * 0.66;
    scene->addLine(zoneRect.left() + 18.0, roadTop, zoneRect.right() - 18.0, roadTop, guidePen)->setZValue(1.0);
    scene->addLine(zoneRect.left() + 18.0, roadBottom, zoneRect.right() - 18.0, roadBottom, guidePen)->setZValue(1.0);

    constexpr int slotsPerRow = 7;
    const double slotGap = 6.0;
    const double slotWidth = (zoneRect.width() - 44.0 - slotGap * (slotsPerRow - 1)) / slotsPerRow;
    const double slotHeight = 72.0;
    for (int slot = 0; slot < slotsPerRow; ++slot) {
        const double x = zoneRect.left() + 22.0 + slot * (slotWidth + slotGap);
        scene->addRect(QRectF(x, zoneRect.top() + 22.0, slotWidth, slotHeight), slotPen, Qt::NoBrush)->setZValue(1.0);
        scene->addRect(QRectF(x, zoneRect.bottom() - slotHeight - 22.0, slotWidth, slotHeight), slotPen, Qt::NoBrush)
            ->setZValue(1.0);
    }

    QFont zoneFont(QStringLiteral("Segoe UI"));
    zoneFont.setPointSizeF(18.0);
    zoneFont.setWeight(QFont::Bold);
    addCenteredLabel(scene, QStringLiteral("ZONE %1").arg(zoneNumber), zoneFont, QColor(QStringLiteral("#73c9f2")),
                     0.86, QPointF(zoneRect.center().x(), zoneRect.top() - 24.0));
    addChannelLabels(scene, zoneRect);
}
}  // namespace

/**
 * @brief        두 물리 CCTV 구역을 하나의 scene에 나란히 구성합니다.
 * @param scene  고정 지도 요소를 추가할 scene
 * @return       전체 scene과 구역별 객체 배치 영역
 */
DigitalTwinMapSceneLayout DemoParkingMapSceneBuilder::build(QGraphicsScene* scene) const {
    if (!scene) {
        return {};
    }

    DigitalTwinMapSceneLayout layout;
    layout.sceneRect = QRectF(0.0, 0.0, sceneWidth, sceneHeight);
    layout.zoneRects = {QRectF(leftZoneLeft, zoneTop, zoneSize, zoneSize),
                        QRectF(rightZoneLeft, zoneTop, zoneSize, zoneSize)};

    scene->setItemIndexMethod(QGraphicsScene::NoIndex);
    scene->setSceneRect(layout.sceneRect.adjusted(-18.0, -18.0, 18.0, 18.0));
    scene->setBackgroundBrush(QColor(QStringLiteral("#08111d")));
    scene->addRect(layout.sceneRect, QPen(QColor(QStringLiteral("#1d2c3f")), 1.0),
                   QBrush(QColor(QStringLiteral("#081421"))));

    addZone(scene, layout.zoneRects[0], 1);
    addZone(scene, layout.zoneRects[1], 2);
    return layout;
}
