#include "ui/DigitalTwinMapSceneBuilder.h"

#include <QBrush>
#include <QColor>
#include <QGraphicsScene>
#include <QLineF>
#include <QPen>
#include <QString>

namespace {
constexpr double sceneWidth = 1000.0;
constexpr double sceneHeight = 520.0;
constexpr double movementAreaLeft = 44.0;
constexpr double movementAreaTop = 54.0;
constexpr double movementAreaWidth = 912.0;
constexpr double movementAreaHeight = 396.0;
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
    QPen slotPen(QColor(QStringLiteral("#40546d")), 1.4);

    scene->addRect(mapRect, QPen(QColor(QStringLiteral("#1d2c3f")), 1.0), QBrush(QColor(QStringLiteral("#0a1523"))));
    scene->addRect(movementRect, wallPen, Qt::NoBrush)->setZValue(1.0);
    scene->addLine(QLineF(72.0, 190.0, 928.0, 190.0), thinLinePen)->setZValue(1.0);
    scene->addLine(QLineF(72.0, 326.0, 928.0, 326.0), thinLinePen)->setZValue(1.0);
    scene->addLine(QLineF(500.0, 74.0, 500.0, 430.0), thinLinePen)->setZValue(1.0);

    for (int i = 0; i < 8; ++i) {
        const double leftX = 94.0 + i * 48.0;
        const double rightX = 566.0 + i * 48.0;
        scene->addRect(QRectF(leftX, 78.0, 40.0, 94.0), slotPen, Qt::NoBrush)->setZValue(1.0);
        scene->addRect(QRectF(rightX, 78.0, 40.0, 94.0), slotPen, Qt::NoBrush)->setZValue(1.0);
        scene->addRect(QRectF(leftX, 346.0, 40.0, 78.0), slotPen, Qt::NoBrush)->setZValue(1.0);
        scene->addRect(QRectF(rightX, 346.0, 40.0, 78.0), slotPen, Qt::NoBrush)->setZValue(1.0);
    }

    auto* titleItem = scene->addSimpleText(QStringLiteral("B1F DEMO MAP"));
    titleItem->setBrush(QColor(QStringLiteral("#53677f")));
    titleItem->setPos(414.0, 238.0);
    titleItem->setScale(1.8);
    titleItem->setZValue(1.0);

    return movementRect;
}
