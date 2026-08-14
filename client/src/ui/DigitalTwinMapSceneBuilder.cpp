#include "ui/DigitalTwinMapSceneBuilder.h"

#include <QBrush>
#include <QColor>
#include <QFont>
// addPath/addSimpleText가 돌려주는 아이템을 직접 다루므로 완전한 타입이 필요하다.
// PCH가 켜져 있으면 없어도 통과하지만 QTCCTV_ENABLE_PCH=OFF에서는 컴파일되지 않는다
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QString>
#include <array>

#include "model/DigitalTwinRuntimeConfig.h"

namespace {
// ---------------------------------------------------------------------------
// 도면 치수 (scene 좌표, 1000x520)
//
// 통로는 커버리지 격자에서 파생시킨다. 구역마다 중심을 지나는 십자 통로를 두고 그 교차점에
// 카메라를 세우므로, 채널 4개가 네 방향(상/우/하/좌) 사분면을 하나씩 맡는다. 통로를 따로
// 정의하면 셀 중심과 어긋나 카메라가 통로 밖에 서게 되므로 반드시 여기서 계산한다.
// ---------------------------------------------------------------------------
constexpr double sceneWidth = 1000.0;
constexpr double sceneHeight = 520.0;

constexpr double wallLeft = 12.0;
constexpr double wallTop = 46.0;
constexpr double wallRight = 988.0;
constexpr double wallBottom = 466.0;

/// 좌측 진출입 순환로. 실측 도면처럼 넉넉한 폭을 준다
constexpr double fieldLeft = 140.0;
/// 우측 계단·승강기 코어와 출차 램프 앞까지가 주차 구획이다
constexpr double fieldRight = 948.0;

/// 상·하단 장치 상태 칩 띠의 중심
constexpr double topStripCenterY = 20.0;
constexpr double bottomStripCenterY = 494.0;
constexpr double statusChipWidth = 116.0;
constexpr double statusChipHeight = 26.0;

/// 물리 CCTV 커버리지 격자. 열은 기둥 그리드, 행은 주 통로에 맞춘다
constexpr int coverageColumnCount = 4;
constexpr int coverageRowCount = 2;
constexpr int coverageCellCount = coverageColumnCount * coverageRowCount;
constexpr int channelsPerZone = 4;
// 설정이 받아들이는 구역 수는 이 도면이 그릴 수 있는 칸 수와 같아야 한다
static_assert(coverageCellCount == digitalTwinMaximumZoneCount);
/// 셀 경계가 외벽·옆 셀과 겹쳐 보이지 않도록 두는 여백. 중심은 그대로 유지된다
constexpr double coverageInset = 4.0;

/// 구역 십자 통로의 폭. 가로·세로 모두 같다
constexpr double crossAisleWidth = 40.0;

/// 주차 구획은 도면 전체에서 같은 규격을 쓴다. 방향만 90도 돌려 쓴다
constexpr double stallShort = 16.0;
constexpr double stallLong = 36.0;
constexpr double stallGap = 3.0;
constexpr double stallPitch = stallShort + stallGap;

const QColor colorFloor(QStringLiteral("#08192a"));
const QColor colorGrid(QStringLiteral("#0e2740"));
const QColor colorWall(QStringLiteral("#7fb4cf"));
const QColor colorNeon(QStringLiteral("#55dcff"));
const QColor colorStall(QStringLiteral("#2c668a"));
const QColor colorAisle(QStringLiteral("#14405a"));
const QColor colorAccessibleFill(QStringLiteral("#123047"));
const QColor colorAccessibleEdge(QStringLiteral("#2b6d92"));
const QColor colorAccessibleGlyph(QStringLiteral("#4e9dc4"));
const QColor colorCore(QStringLiteral("#2a5c78"));
const QColor colorReserved(QStringLiteral("#1d3f55"));
const QColor colorTextDim(QStringLiteral("#7ba3bd"));
const QColor colorEntry(QStringLiteral("#5cff80"));
const QColor colorExit(QStringLiteral("#ff9d5c"));

QFont sceneFont(double pointSize, QFont::Weight weight = QFont::Normal) {
    QFont font(QStringLiteral("Segoe UI"));
    font.setPointSizeF(pointSize);
    font.setWeight(weight);
    return font;
}

void addCenteredLabel(QGraphicsScene* scene, const QString& text, const QFont& font, const QColor& color,
                      double opacity, const QPointF& center, double zValue = 6.0) {
    auto* label = scene->addSimpleText(text, font);
    label->setBrush(color);
    label->setOpacity(opacity);
    label->setZValue(zValue);
    const QRectF bounds = label->boundingRect();
    label->setPos(center.x() - bounds.width() * 0.5, center.y() - bounds.height() * 0.5);
}

/**
 * @brief        같은 path를 굵기와 투명도를 달리해 겹쳐 네온 번짐을 만듭니다.
 *
 * @details QGraphicsDropShadowEffect는 아이템마다 오프스크린 렌더를 강제해서 맵 전체에 걸면
 *          프레임이 급격히 떨어진다. 정적 도면이므로 stroke를 세 겹 겹치는 쪽이 훨씬 싸다.
 */
void addNeonPath(QGraphicsScene* scene, const QPainterPath& path, const QColor& color, double width, double zValue,
                 const QBrush& brush = Qt::NoBrush) {
    struct GlowLayer {
        double widthScale;
        int alpha;
    };
    constexpr std::array<GlowLayer, 3> layers = {{{5.0, 20}, {2.6, 50}, {1.0, 255}}};

    for (const GlowLayer& layer : layers) {
        QColor layerColor = color;
        layerColor.setAlpha(layer.alpha);
        QPen pen(layerColor, width * layer.widthScale);
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        // 채우기는 가장 안쪽 stroke에서 한 번만 한다
        auto* item = scene->addPath(path, pen, layer.widthScale > 1.0 ? QBrush(Qt::NoBrush) : brush);
        item->setZValue(zValue);
    }
}

double coverageColumnWidth() { return (fieldRight - fieldLeft) / coverageColumnCount; }

double coverageRowHeight() { return (wallBottom - wallTop) / coverageRowCount; }

/** @brief 커버리지 격자에서 물리 CCTV 한 대가 담당하는 사각형을 돌려줍니다. */
QRectF coverageCell(int cellIndex) {
    const int column = cellIndex % coverageColumnCount;
    const int row = cellIndex / coverageColumnCount;
    return QRectF(fieldLeft + column * coverageColumnWidth() + coverageInset,
                  wallTop + row * coverageRowHeight() + coverageInset, coverageColumnWidth() - coverageInset * 2.0,
                  coverageRowHeight() - coverageInset * 2.0);
}

/** @brief 구역별 장치 상태 칩이 놓일 자리를 돌려줍니다. 윗줄 구역은 위, 아랫줄 구역은 아래다. */
QRectF coverageStatusSlot(int cellIndex) {
    const QRectF cell = coverageCell(cellIndex);
    const double centerY = cellIndex < coverageColumnCount ? topStripCenterY : bottomStripCenterY;
    return QRectF(cell.center().x() - statusChipWidth * 0.5, centerY - statusChipHeight * 0.5, statusChipWidth,
                  statusChipHeight);
}

/**
 * @brief              십자 통로가 잘라 낸 사분면 하나를 돌려줍니다.
 * @param cell         구역 사각형
 * @param quadrantIndex 0=좌상, 1=우상, 2=좌하, 3=우하
 */
QRectF cellQuadrant(const QRectF& cell, int quadrantIndex) {
    const QPointF center = cell.center();
    const double half = crossAisleWidth * 0.5;
    const double left = (quadrantIndex % 2) == 0 ? cell.left() : center.x() + half;
    const double right = (quadrantIndex % 2) == 0 ? center.x() - half : cell.right();
    const double top = quadrantIndex < 2 ? cell.top() : center.y() + half;
    const double bottom = quadrantIndex < 2 ? center.y() - half : cell.bottom();
    return QRectF(left, top, right - left, bottom - top);
}

int quadrantStallColumns(const QRectF& quadrant) {
    return static_cast<int>((quadrant.width() + stallGap) / stallPitch);
}

int quadrantStallRows(const QRectF& quadrant) { return static_cast<int>(quadrant.height() / stallLong); }

/**
 * @brief 사분면 안 주차 구획 하나의 위치를 돌려줍니다.
 *
 * @details 차는 가로 통로에서 진입하므로 구획은 통로에 붙여 채운다. 위쪽 사분면은 아래를
 *          기준으로, 아래쪽 사분면은 위를 기준으로 쌓는다. 남는 여백은 항상 통로 반대편이다.
 */
QRectF quadrantStallRect(const QRectF& quadrant, bool hugBottom, int row, int column) {
    const double blockWidth = quadrantStallColumns(quadrant) * stallPitch - stallGap;
    const double x = quadrant.left() + (quadrant.width() - blockWidth) * 0.5 + column * stallPitch;
    const double y = hugBottom ? quadrant.bottom() - (row + 1) * stallLong : quadrant.top() + row * stallLong;
    return QRectF(x, y, stallShort, stallLong);
}

/** @brief 사분면을 같은 규격의 주차 구획으로 채웁니다. */
void appendQuadrantStalls(QPainterPath& path, const QRectF& quadrant, bool hugBottom) {
    const int columns = quadrantStallColumns(quadrant);
    const int rows = quadrantStallRows(quadrant);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            path.addRect(quadrantStallRect(quadrant, hugBottom, row, column));
        }
    }
}

/** @brief 사각형을 45도 빗금으로 채웁니다(계단·승강기 코어 표기). */
void appendHatch(QPainterPath& path, const QRectF& rect, double spacing) {
    path.addRect(rect);
    for (double offset = spacing; offset < rect.width() + rect.height(); offset += spacing) {
        const double x0 = rect.left() + qMin(offset, rect.width());
        const double y0 = rect.top() + qMax(0.0, offset - rect.width());
        const double x1 = rect.left() + qMax(0.0, offset - rect.height());
        const double y1 = rect.top() + qMin(offset, rect.height());
        path.moveTo(x0, y0);
        path.lineTo(x1, y1);
    }
}

/** @brief 장애인 주차 표식(머리 + 바퀴)을 작은 크기로 그립니다. */
void appendAccessibleGlyph(QPainterPath& path, const QPointF& center) {
    path.addEllipse(center + QPointF(0.0, -5.0), 1.5, 1.5);
    path.addEllipse(center + QPointF(0.0, 1.5), 3.6, 3.6);
}

/** @brief 승강기·장애인 주차 코어가 들어가는 사분면인지 확인합니다. */
bool isServiceQuadrant(int cellIndex, int quadrantIndex) {
    // 실측 도면과 같이 마지막 구역의 우하단을 서비스 코어로 비워 둔다
    return cellIndex == coverageCellCount - 1 && quadrantIndex == 3;
}

/** @brief 바닥 격자와 외벽, 진출입 램프를 그립니다. */
void addShell(QGraphicsScene* scene) {
    const QRectF floorRect(wallLeft, wallTop, wallRight - wallLeft, wallBottom - wallTop);
    scene->addRect(floorRect, QPen(Qt::NoPen), QBrush(colorFloor))->setZValue(0.2);

    // 실측 도면 느낌을 내는 옅은 방안지 격자
    QPainterPath gridPath;
    for (double x = wallLeft; x <= wallRight; x += 20.0) {
        gridPath.moveTo(x, wallTop);
        gridPath.lineTo(x, wallBottom);
    }
    for (double y = wallTop; y <= wallBottom; y += 20.0) {
        gridPath.moveTo(wallLeft, y);
        gridPath.lineTo(wallRight, y);
    }
    scene->addPath(gridPath, QPen(colorGrid, 0.6), Qt::NoBrush)->setZValue(0.3);

    // 외벽과 좌측 순환로·우측 코어 경계. 도면선이므로 네온보다 차분한 흰빛으로 둔다
    QPainterPath wallPath;
    wallPath.addRect(floorRect);
    wallPath.moveTo(fieldLeft, wallTop);
    wallPath.lineTo(fieldLeft, wallBottom);
    wallPath.moveTo(fieldRight, wallTop);
    wallPath.lineTo(fieldRight, wallBottom);
    scene->addPath(wallPath, QPen(colorWall, 1.6), Qt::NoBrush)->setZValue(4.0);

    // 진입: 좌상단에서 순환로로 들어온다
    QPainterPath entryPath;
    entryPath.moveTo(wallLeft + 18.0, 64.0);
    entryPath.lineTo(fieldLeft - 22.0, 64.0);
    entryPath.moveTo(fieldLeft - 34.0, 56.0);
    entryPath.lineTo(fieldLeft - 22.0, 64.0);
    entryPath.lineTo(fieldLeft - 34.0, 72.0);
    addNeonPath(scene, entryPath, colorEntry, 1.3, 5.0);
    addCenteredLabel(scene, QStringLiteral("IN"), sceneFont(10.0, QFont::Bold), colorEntry, 0.92,
                     QPointF((wallLeft + fieldLeft) * 0.5, 82.0));

    // 출차: 아래쪽 가로 통로가 우측 벽 노치로 이어진다
    const double exitY = coverageCell(coverageColumnCount).center().y();
    QPainterPath exitPath;
    exitPath.moveTo(fieldRight + 6.0, exitY);
    exitPath.lineTo(wallRight - 8.0, exitY);
    exitPath.moveTo(wallRight - 18.0, exitY - 7.0);
    exitPath.lineTo(wallRight - 8.0, exitY);
    exitPath.lineTo(wallRight - 18.0, exitY + 7.0);
    addNeonPath(scene, exitPath, colorExit, 1.3, 5.0);
    addCenteredLabel(scene, QStringLiteral("OUT"), sceneFont(9.5, QFont::Bold), colorExit, 0.92,
                     QPointF((fieldRight + wallRight) * 0.5, exitY + 16.0));

    // 좌측 순환로: 주행 방향선과 요금 정산 부스
    QPainterPath rampPath;
    const double rampCenterX = (wallLeft + fieldLeft) * 0.5;
    rampPath.moveTo(rampCenterX, 100.0);
    rampPath.lineTo(rampCenterX, wallBottom - 90.0);
    QPen rampPen(colorAisle, 1.0, Qt::DashLine);
    rampPen.setDashPattern({7.0, 8.0});
    scene->addPath(rampPath, rampPen, Qt::NoBrush)->setZValue(0.8);

    QPainterPath boothPath;
    boothPath.addRect(QRectF(wallLeft + 10.0, 246.0, 34.0, 30.0));
    scene->addPath(boothPath, QPen(colorCore, 1.0), Qt::NoBrush)->setZValue(1.1);
}

/** @brief 구역별 십자 통로와 그 사분면을 채우는 주차 구획을 그립니다. */
void addParkingField(QGraphicsScene* scene) {
    QPainterPath stallPath;
    QPainterPath accessiblePath;
    QPainterPath accessibleGlyphPath;

    for (int cellIndex = 0; cellIndex < coverageCellCount; ++cellIndex) {
        const QRectF cell = coverageCell(cellIndex);
        for (int quadrantIndex = 0; quadrantIndex < 4; ++quadrantIndex) {
            if (isServiceQuadrant(cellIndex, quadrantIndex)) {
                continue;
            }

            const QRectF quadrant = cellQuadrant(cell, quadrantIndex);
            const bool hugBottom = quadrantIndex < 2;
            appendQuadrantStalls(stallPath, quadrant, hugBottom);

            // 장애인 주차: 승강기·계단 코어에서 가까운 구역의 통로 쪽 구획 두 자리
            if (cellIndex == 2 && quadrantIndex == 0) {
                for (int column = 0; column < 2; ++column) {
                    const QRectF stall = quadrantStallRect(quadrant, hugBottom, 0, column);
                    accessiblePath.addRect(stall);
                    appendAccessibleGlyph(accessibleGlyphPath, stall.center());
                }
            }
        }
    }
    scene->addPath(stallPath, QPen(colorStall, 1.0), Qt::NoBrush)->setZValue(1.0);

    // 십자 통로 중심선. 가로선은 구역 행 중심, 세로선은 구역 열 중심을 지난다
    QPainterPath aislePath;
    for (int row = 0; row < coverageRowCount; ++row) {
        const double y = coverageCell(row * coverageColumnCount).center().y();
        aislePath.moveTo(fieldLeft + 12.0, y);
        aislePath.lineTo(fieldRight - 12.0, y);
    }
    for (int column = 0; column < coverageColumnCount; ++column) {
        const double x = coverageCell(column).center().x();
        aislePath.moveTo(x, wallTop + 12.0);
        aislePath.lineTo(x, wallBottom - 12.0);
    }
    QPen aislePen(colorAisle, 1.0, Qt::DashLine);
    aislePen.setDashPattern({6.0, 7.0});
    scene->addPath(aislePath, aislePen, Qt::NoBrush)->setZValue(0.8);

    // 기둥: 구역 경계와 가로 통로가 만나는 지점
    QPainterPath columnPath;
    for (int column = 0; column <= coverageColumnCount; ++column) {
        const double x = fieldLeft + column * coverageColumnWidth();
        for (int row = 0; row < coverageRowCount; ++row) {
            const double y = coverageCell(row * coverageColumnCount).center().y();
            columnPath.addRect(QRectF(x - 2.5, y - 2.5, 5.0, 5.0));
        }
    }
    scene->addPath(columnPath, QPen(colorCore, 0.9), QBrush(colorFloor))->setZValue(1.3);

    scene->addPath(accessiblePath, QPen(colorAccessibleEdge, 1.0), QBrush(colorAccessibleFill))->setZValue(1.15);
    scene->addPath(accessibleGlyphPath, QPen(colorAccessibleGlyph, 0.9), Qt::NoBrush)->setZValue(1.2);

    // 계단·승강기 코어
    QPainterPath corePath;
    appendHatch(corePath, QRectF(wallLeft + 12.0, wallBottom - 62.0, 74.0, 48.0), 9.0);
    appendHatch(corePath, QRectF(fieldRight + 8.0, 150.0, 24.0, 42.0), 8.0);
    appendHatch(corePath, QRectF(fieldRight + 8.0, 250.0, 24.0, 42.0), 8.0);
    const QRectF serviceQuadrant = cellQuadrant(coverageCell(coverageCellCount - 1), 3);
    appendHatch(corePath, serviceQuadrant.adjusted(8.0, 8.0, -8.0, -8.0), 9.0);
    scene->addPath(corePath, QPen(colorCore, 1.0), Qt::NoBrush)->setZValue(1.1);
    addCenteredLabel(scene, QStringLiteral("EV"), sceneFont(7.0, QFont::DemiBold), colorTextDim, 0.6,
                     serviceQuadrant.center(), 1.4);
}

/** @brief 기둥 그리드 번호를 도면 네 변에 표시합니다. */
void addColumnGridMarkers(QGraphicsScene* scene) {
    constexpr double markerRadius = 6.5;
    const QFont markerFont = sceneFont(6.0, QFont::DemiBold);

    const auto addMarker = [scene, &markerFont](const QPointF& center, int number) {
        scene
            ->addEllipse(
                QRectF(center.x() - markerRadius, center.y() - markerRadius, markerRadius * 2.0, markerRadius * 2.0),
                QPen(colorReserved, 1.0), Qt::NoBrush)
            ->setZValue(5.0);
        addCenteredLabel(scene, QString::number(number), markerFont, colorTextDim, 0.8, center, 5.1);
    };

    for (int column = 0; column <= coverageColumnCount; ++column) {
        const double x = fieldLeft + column * coverageColumnWidth();
        addMarker(QPointF(x, wallTop - 11.0), column + 1);
        addMarker(QPointF(x, wallBottom + 11.0), column + 1 + coverageColumnCount + 1);
    }

    // 행 번호는 좌우 같은 값을 쓴다(도면 그리드 관례). 출차 램프와 겹치는 자리만 건너뛴다
    const double exitY = coverageCell(coverageColumnCount).center().y();
    for (int row = 0; row <= coverageRowCount; ++row) {
        const double y = wallTop + row * coverageRowHeight();
        addMarker(QPointF(wallLeft - 11.0, y), row + 1);
        if (qAbs(y - exitY) > 30.0) {
            addMarker(QPointF(wallRight + 11.0, y), row + 1);
        }
    }
}

/** @brief 활성 CCTV 구역의 커버리지 경계와 채널 경계를 그립니다. */
void addActiveZone(QGraphicsScene* scene, const QRectF& cell) {
    QPainterPath borderPath;
    borderPath.addRect(cell);
    QColor fill = colorNeon;
    fill.setAlpha(14);
    addNeonPath(scene, borderPath, colorNeon, 1.4, 3.0, QBrush(fill));

    // 채널 경계: 카메라가 십자 통로 교차점에서 상/우/하/좌를 보므로 경계는 두 대각선이다
    QPainterPath channelPath;
    channelPath.moveTo(cell.topLeft());
    channelPath.lineTo(cell.bottomRight());
    channelPath.moveTo(cell.topRight());
    channelPath.lineTo(cell.bottomLeft());
    QColor channelColor = colorNeon;
    channelColor.setAlpha(95);
    QPen channelPen(channelColor, 1.0, Qt::DashLine);
    channelPen.setDashPattern({4.0, 6.0});
    scene->addPath(channelPath, channelPen, Qt::NoBrush)->setZValue(3.2);

    // 채널 이름표는 각 사분면의 통로 쪽 끝, 카메라 아이콘을 피한 자리에 둔다
    const QPointF center = cell.center();
    const std::array<QPointF, channelsPerZone> centers = {{
        {center.x(), cell.top() + cell.height() / 6.0},
        {cell.right() - cell.width() / 6.0, center.y()},
        {center.x(), cell.bottom() - cell.height() / 6.0},
        {cell.left() + cell.width() / 6.0, center.y()},
    }};
    for (int localChannel = 0; localChannel < channelsPerZone; ++localChannel) {
        addCenteredLabel(scene, QStringLiteral("CH%1").arg(localChannel + 1, 2, 10, QLatin1Char('0')),
                         sceneFont(7.0, QFont::DemiBold), colorTextDim, 0.72, centers[localChannel]);
    }
}

/** @brief 아직 CCTV가 없는 커버리지 자리를 확장용으로 남겨 표시합니다. */
void addReservedZone(QGraphicsScene* scene, const QRectF& cell, int zoneNumber) {
    QPen reservedPen(colorReserved, 1.0, Qt::DashLine);
    reservedPen.setDashPattern({3.0, 7.0});
    scene->addRect(cell, reservedPen, Qt::NoBrush)->setZValue(2.0);
    // 카메라가 없는 자리이므로 십자 통로 교차점이 비어 있다. 구획선과 겹치지 않는 유일한 지점이다
    addCenteredLabel(scene, QStringLiteral("구역 %1 · 확장 예정").arg(zoneNumber), sceneFont(7.0), colorTextDim, 0.32,
                     cell.center());
}

/**
 * @brief 구역별 장치 상태 칩의 배경과 이름표를 그립니다.
 *
 * @details 아이콘은 DeviceStatusMapOverlay가 이 칩 안에 구역당 한 쌍만 올린다. 활성 구역은
 *          또렷하게, 아직 CCTV가 없는 자리는 흐리게 두어 확장 여부가 한눈에 보이게 한다.
 */
void addStatusChip(QGraphicsScene* scene, const QRectF& slot, int zoneNumber, bool active) {
    QColor plate(QStringLiteral("#082032"));
    plate.setAlpha(active ? 230 : 120);
    QColor edge = active ? colorNeon : colorReserved;
    edge.setAlpha(active ? 190 : 110);
    scene->addRect(slot, QPen(edge, 1.0), QBrush(plate))->setZValue(5.0);

    auto* label = scene->addSimpleText(QStringLiteral("구역 %1").arg(zoneNumber), sceneFont(7.5, QFont::DemiBold));
    label->setBrush(active ? colorNeon : colorTextDim);
    label->setOpacity(active ? 0.95 : 0.42);
    label->setZValue(5.2);
    label->setPos(slot.left() + 9.0, slot.center().y() - label->boundingRect().height() * 0.5);
}
}  // namespace

/**
 * @brief                  주차장 도면 한 장을 네온 스타일로 구성하고 CCTV 커버리지 격자를 올립니다.
 * @param scene            고정 지도 요소를 추가할 scene
 * @param activeZoneCount  CCTV가 붙어 있는 구역 수
 * @return                 전체 scene, 활성 CCTV 구역, 구역별 장치 상태 칩 자리
 *
 * @details 커버리지는 4열 x 2행 격자이고 앞에서부터 activeZoneCount칸만 실제 CCTV가 있다.
 *          나머지는 확장 자리로 비워 둔다. 구역을 늘리는 것은 app_config.json의 video.areas와
 *          digitalTwin.world.zones를 같은 개수로 맞추는 일이고, 이 함수는 그 결과를 따른다.
 */
DigitalTwinMapSceneLayout DemoParkingMapSceneBuilder::build(QGraphicsScene* scene, int activeZoneCount) const {
    if (!scene) {
        return {};
    }

    const int zoneCount = qBound(1, activeZoneCount, coverageCellCount);
    DigitalTwinMapSceneLayout layout;
    layout.sceneRect = QRectF(0.0, 0.0, sceneWidth, sceneHeight);
    layout.zoneRects.reserve(zoneCount);
    layout.zoneStatusSlots.reserve(zoneCount);
    for (int zoneIndex = 0; zoneIndex < zoneCount; ++zoneIndex) {
        layout.zoneRects.append(coverageCell(zoneIndex));
        layout.zoneStatusSlots.append(coverageStatusSlot(zoneIndex));
    }

    scene->setItemIndexMethod(QGraphicsScene::NoIndex);
    scene->setSceneRect(layout.sceneRect.adjusted(-18.0, -18.0, 18.0, 18.0));
    scene->setBackgroundBrush(QColor(QStringLiteral("#061726")));

    addShell(scene);
    addParkingField(scene);
    addColumnGridMarkers(scene);

    for (int cellIndex = 0; cellIndex < coverageCellCount; ++cellIndex) {
        const bool active = cellIndex < zoneCount;
        if (active) {
            addActiveZone(scene, layout.zoneRects[cellIndex]);
        } else {
            addReservedZone(scene, coverageCell(cellIndex), cellIndex + 1);
        }
        addStatusChip(scene, coverageStatusSlot(cellIndex), cellIndex + 1, active);
    }

    return layout;
}
