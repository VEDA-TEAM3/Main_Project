pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import "Theme.js" as Theme

// 설비실 한 칸. 꺾인 모서리까지 그리려고 사각형이 아니라 outline 폴리곤으로 받습니다.
//
// 입체감은 **벽이 냅니다.** 방 바닥은 평평한 콘크리트라 거의 균일하게 두고, 두께와 그림자는
// 방을 둘러싼 PlanWalls가 맡습니다. 바닥까지 진하게 칠하면 벽이 서 있는 게 아니라 방 전체가
// 부풀어 오른 덩어리로 보입니다.
Item {
    id: root

    /** ParkingPlan.js가 만든 방. outline은 [x0,y0,x1,y1,...] 평평한 좌표 목록입니다. */
    required property var room
    /** 이름표 글자 크기(도면 단위) */
    property real labelSize: 9.5

    anchors.fill: parent

    /** 평평한 좌표 목록을 Shape가 받는 점 배열로 바꿉니다. 닫힌 도형이라 첫 점을 끝에 다시 붙입니다. */
    readonly property var outlinePoints: {
        var points = [];
        for (var i = 0; i + 1 < root.room.outline.length; i += 2) {
            points.push(Qt.point(root.room.outline[i], root.room.outline[i + 1]));
        }
        points.push(Qt.point(root.room.outline[0], root.room.outline[1]));
        return points;
    }

    // 방 바닥면
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: "transparent"
            startX: root.room.outline[0]
            startY: root.room.outline[1]

            // 기울기는 아주 좁게 줍니다. 도면에서 방 **바닥**은 평평한 콘크리트라 거의 균일하고,
            // 입체감은 방을 둘러싼 벽이 냅니다. 바닥에 진한 기울기를 주면 벽이 서 있는 게 아니라
            // 방 전체가 부풀어 오른 것처럼 보입니다
            fillGradient: LinearGradient {
                x1: root.room.x
                y1: root.room.y
                x2: root.room.x + root.room.w
                y2: root.room.y + root.room.h

                GradientStop {
                    position: 0.0
                    color: Theme.mapRoomLit
                }
                GradientStop {
                    position: 1.0
                    color: Theme.mapRoom
                }
            }

            PathPolyline {
                path: root.outlinePoints
            }
        }
    }

    // 위쪽 벽이 방 안으로 드리우는 그늘. 벽이 바닥보다 높이 서 있는 것처럼 읽히게 합니다.
    // 진하게 주면 방마다 위쪽이 시커메져 도면이 아니라 그림자 그림이 되므로 얕게만 깝니다
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: "transparent"
            startX: root.room.outline[0]
            startY: root.room.outline[1]

            fillGradient: LinearGradient {
                x1: root.room.x
                y1: root.room.y
                x2: root.room.x
                y2: root.room.y + root.room.h * 0.22

                GradientStop {
                    position: 0.0
                    color: Qt.rgba(0, 0, 0, 0.16)
                }
                GradientStop {
                    position: 1.0
                    color: Qt.rgba(0, 0, 0, 0)
                }
            }

            PathPolyline {
                path: root.outlinePoints
            }
        }
    }

    // ── 설비 기호 ──────────────────────────────────────────────────
    // 벽보다 **먼저** 그립니다. 벽에 등을 붙인 장비가 벽 위로 삐져나오면 방 밖으로 튀어나온
    // 것처럼 보이는데, 순서를 이렇게 두면 벽이 그 자리를 덮어 자연히 맞물립니다.
    Repeater {
        model: root.room.fixtures

        Item {
            id: unit

            required property var modelData

            readonly property real corner: unit.modelData.round === true
                                           ? Math.min(unit.modelData.w, unit.modelData.h) / 2 : 1.0

            // 바닥 그림자. 벽·스토퍼와 같은 왼쪽 위 광원이라야 따로 놀지 않습니다
            Rectangle {
                x: unit.modelData.x + 0.8
                y: unit.modelData.y + 1.3
                width: unit.modelData.w
                height: unit.modelData.h
                radius: unit.corner
                color: Theme.mapWallShadow
                opacity: 0.30
            }

            Rectangle {
                x: unit.modelData.x
                y: unit.modelData.y
                width: unit.modelData.w
                height: unit.modelData.h
                radius: unit.corner
                color: unit.modelData.solid === true ? Theme.mapRoomInk : Theme.mapRoomShade
                opacity: unit.modelData.solid === true ? 0.72 : 1.0
                border.width: 0.9
                border.color: Theme.mapRoomInk
            }

            // 칸 나눔. 긴 축을 가르므로 배전반·수배전반처럼 같은 장비가 줄지어 선 한 벌로 읽힙니다
            Repeater {
                model: Math.max(0, unit.modelData.divisions - 1)

                Rectangle {
                    id: slot

                    required property int index

                    readonly property bool vertical: unit.modelData.w >= unit.modelData.h
                    readonly property real ratio: (slot.index + 1) / unit.modelData.divisions

                    x: slot.vertical ? unit.modelData.x + unit.modelData.w * slot.ratio - 0.35
                                     : unit.modelData.x + 1.5
                    y: slot.vertical ? unit.modelData.y + 1.5
                                     : unit.modelData.y + unit.modelData.h * slot.ratio - 0.35
                    width: slot.vertical ? 0.7 : unit.modelData.w - 3
                    height: slot.vertical ? unit.modelData.h - 3 : 0.7
                    color: Theme.mapRoomInk
                    opacity: 0.6
                }
            }

            // 승강로 대각 X. Shape 없이 회전한 얇은 사각형 둘이면 충분합니다
            Repeater {
                model: unit.modelData.cross === true ? [1, -1] : []

                Rectangle {
                    id: diagonal

                    required property int modelData

                    readonly property real span: Math.sqrt(unit.modelData.w * unit.modelData.w
                                                           + unit.modelData.h * unit.modelData.h) - 2.5

                    x: unit.modelData.x + unit.modelData.w / 2 - diagonal.span / 2
                    y: unit.modelData.y + unit.modelData.h / 2 - 0.35
                    width: diagonal.span
                    height: 0.7
                    rotation: diagonal.modelData * Math.atan2(unit.modelData.h, unit.modelData.w) * 180 / Math.PI
                    color: Theme.mapRoomInk
                    opacity: 0.55
                }
            }
        }
    }

    PlanWalls {
        paths: [root.outlinePoints]
        barWidth: 4.0
        relief: 0.8
    }

    // 문 개구부. 벽 바를 바닥색으로 덮어 뚫린 자리로 보이게 합니다
    Repeater {
        model: root.room.doors

        Rectangle {
            required property var modelData

            x: modelData.gapX
            y: modelData.gapY
            width: modelData.gapW
            height: modelData.gapH
            color: Theme.mapFloor
        }
    }

    Repeater {
        model: root.room.doors

        PlanDoor {
            required property var modelData

            door: modelData
        }
    }

    // 계단참을 가르는 안쪽 벽
    Loader {
        active: root.room.landingWall !== null && root.room.landingWall !== undefined
        anchors.fill: parent

        sourceComponent: PlanWalls {
            paths: [[Qt.point(root.room.landingWall.x1, root.room.landingWall.y1),
                     Qt.point(root.room.landingWall.x2, root.room.landingWall.y2)]]
            barWidth: 3.0
            relief: 0.8
        }
    }

    // 계단은 단판을 긋고 가운데로 올라가는 방향을 얹습니다. 실제 도면의 계단실 표기입니다
    Loader {
        // 승강기 코어에는 stair 자리가 아예 없으므로 undefined를 bool로 넘기지 않도록 비교한다
        active: root.room.stair === true
        anchors.fill: parent

        sourceComponent: Shape {
            id: stairGlyph

            readonly property var box: root.room.stairBox
            readonly property real midX: stairGlyph.box.x + stairGlyph.box.w / 2

            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            opacity: 0.8

            // 계단 옆판
            ShapePath {
                strokeColor: Theme.mapRoomInk
                strokeWidth: 1.2
                fillColor: "transparent"
                joinStyle: ShapePath.MiterJoin

                PathPolyline {
                    path: [Qt.point(stairGlyph.box.x, stairGlyph.box.y),
                           Qt.point(stairGlyph.box.x + stairGlyph.box.w, stairGlyph.box.y),
                           Qt.point(stairGlyph.box.x + stairGlyph.box.w, stairGlyph.box.y + stairGlyph.box.h),
                           Qt.point(stairGlyph.box.x, stairGlyph.box.y + stairGlyph.box.h),
                           Qt.point(stairGlyph.box.x, stairGlyph.box.y)]
                }
            }

            // 단판
            ShapePath {
                strokeColor: Theme.mapRoomInk
                strokeWidth: 0.8
                fillColor: "transparent"

                PathMultiline {
                    paths: {
                        var lines = [];
                        var step = stairGlyph.box.h / 11;
                        for (var i = 1; i < 11; ++i) {
                            var y = stairGlyph.box.y + i * step;
                            lines.push([Qt.point(stairGlyph.box.x, y),
                                        Qt.point(stairGlyph.box.x + stairGlyph.box.w, y)]);
                        }
                        return lines;
                    }
                }
            }

            // 올라가는 방향
            ShapePath {
                strokeColor: Theme.mapRoomInk
                strokeWidth: 1.5
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                startX: stairGlyph.midX
                startY: stairGlyph.box.y + stairGlyph.box.h - 3

                PathLine {
                    x: stairGlyph.midX
                    y: stairGlyph.box.y + 3
                }
                PathMove {
                    x: stairGlyph.midX - 4.5
                    y: stairGlyph.box.y + 10
                }
                PathLine {
                    x: stairGlyph.midX
                    y: stairGlyph.box.y + 3
                }
                PathLine {
                    x: stairGlyph.midX + 4.5
                    y: stairGlyph.box.y + 10
                }
            }
        }
    }

    // 이름이 방보다 길면 글자를 줄여 넣습니다. 흘려 보내면 이름이 복도로 튀어나가 옆 방 벽에 얹힙니다
    Text {
        x: root.room.x + 5
        y: root.room.labelAtTop === true ? root.room.y + 7 : root.room.y + root.room.h / 2 - height / 2
        width: root.room.w - 10
        horizontalAlignment: Text.AlignHCenter
        visible: root.room.name.length > 0
        text: root.room.name
        color: Theme.mapRoomInk
        font.family: Theme.fontFamily
        font.pixelSize: root.labelSize
        font.weight: Font.DemiBold
        font.letterSpacing: 0.3
        fontSizeMode: Text.HorizontalFit
        minimumPixelSize: 6
    }
}
