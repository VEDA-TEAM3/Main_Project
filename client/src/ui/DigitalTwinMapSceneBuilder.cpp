#include "ui/DigitalTwinMapSceneBuilder.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QLineF>
#include <QPen>
#include <QPointF>
#include <QString>
#include <array>

namespace {
constexpr double sceneWidth = 1000.0;
constexpr double sceneHeight = 520.0;
constexpr double movementAreaLeft = 44.0;
constexpr double movementAreaTop = 54.0;
constexpr double movementAreaWidth = 912.0;
constexpr double movementAreaHeight = 396.0;
constexpr double channelLabelZValue = 2.0;
constexpr double channelGuideZValue = 1.25;
const QColor channelLabelColor(QStringLiteral("#72afd2"));
const std::array<QPointF, 4> channelLabelCenters = {
    QPointF(500.0, 145.0),
    QPointF(240.0, 252.0),
    QPointF(500.0, 359.0),
    QPointF(760.0, 252.0),
};

/**
 * @brief          지도 구역 중앙에 낮은 강조도의 채널 이름을 배치합니다.
 * @param scene    채널 이름을 추가할 장면
 * @param channel  1부터 시작하는 표시 채널 번호
 * @param center   텍스트 중심 좌표
 */
void addChannelLabel(QGraphicsScene* scene, int channel, const QPointF& center) {
    QFont font(QStringLiteral("Segoe UI"));
    font.setPointSizeF(30.0);
    font.setWeight(QFont::Bold);

    auto* label = scene->addSimpleText(QStringLiteral("CH %1").arg(channel, 2, 10, QLatin1Char('0')), font);
    label->setBrush(channelLabelColor);
    label->setOpacity(0.26);
    label->setZValue(channelLabelZValue);
    const QRectF bounds = label->boundingRect();
    label->setPos(center.x() - bounds.center().x(), center.y() - bounds.center().y());
}
}  // namespace

/**
 * @brief        데모 주차장 맵의 고정 배경 요소를 scene에 구성합니다.
 * @param scene  맵 아이템을 추가할 QGraphicsScene
 * @return       객체 좌표 변환 기준으로 사용할 맵 영역
 */
QRectF DemoParkingMapSceneBuilder::build(QGraphicsScene* scene) const {
    if (!scene) {
        return {};
    }

    const QRectF mapRect(0.0, 0.0, sceneWidth, sceneHeight);
    const QRectF movementRect(movementAreaLeft, movementAreaTop, movementAreaWidth, movementAreaHeight);

    scene->setItemIndexMethod(QGraphicsScene::NoIndex);
    scene->setSceneRect(mapRect.adjusted(-24.0, -24.0, 24.0, 24.0));
    scene->setBackgroundBrush(QColor(QStringLiteral("#08111d")));

    QPen wallPen(QColor(QStringLiteral("#3a4c63")), 3.0);
    QPen thinLinePen(QColor(QStringLiteral("#24364c")), 1.4);
    QPen slotPen(QColor(QStringLiteral("#24364c")), 1.4);

    scene->addRect(mapRect, QPen(QColor(QStringLiteral("#1d2c3f")), 1.0), QBrush(QColor(QStringLiteral("#0a1523"))));
    scene->addRect(movementRect, wallPen, Qt::NoBrush)->setZValue(1.0);
    scene->addLine(QLineF(72.0, 190.0, 928.0, 190.0), thinLinePen)->setZValue(1.0);
    scene->addLine(QLineF(72.0, 326.0, 928.0, 326.0), thinLinePen)->setZValue(1.0);

    QPen channelGuidePen(QColor(QStringLiteral("#31475d")), 1.2, Qt::SolidLine);
    scene->addLine(QLineF(movementRect.topLeft(), movementRect.bottomRight()), channelGuidePen)
        ->setZValue(channelGuideZValue);
    scene->addLine(QLineF(movementRect.topRight(), movementRect.bottomLeft()), channelGuidePen)
        ->setZValue(channelGuideZValue);

    for (int i = 0; i < 8; ++i) {
        const double leftX = 94.0 + i * 48.0;
        const double rightX = 566.0 + i * 48.0;
        scene->addRect(QRectF(leftX, 78.0, 40.0, 94.0), slotPen, Qt::NoBrush)->setZValue(1.0);
        scene->addRect(QRectF(rightX, 78.0, 40.0, 94.0), slotPen, Qt::NoBrush)->setZValue(1.0);
        scene->addRect(QRectF(leftX, 346.0, 40.0, 78.0), slotPen, Qt::NoBrush)->setZValue(1.0);
        scene->addRect(QRectF(rightX, 346.0, 40.0, 78.0), slotPen, Qt::NoBrush)->setZValue(1.0);
    }

    for (int channelIndex = 0; channelIndex < static_cast<int>(channelLabelCenters.size()); ++channelIndex) {
        addChannelLabel(scene, channelIndex + 1, channelLabelCenters[channelIndex]);
    }

    return movementRect;
}
