pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import "Theme.js" as Theme
import "ParkingPlan.js" as Plan

// 디지털 트윈 2D 맵.
//
// 도면 기하는 전부 ParkingPlan.js가 만들고, C++은 살아 있는 값(객체·채널 위험·장치 상태)만
// 평평한 배열로 넘깁니다. 구역 사각형도 이쪽이 원본이라 C++은 objectAreas를 읽어 씁니다.
//
// 도면은 plan 단위로 그리고 stage 하나에만 배율을 겁니다. 자식마다 좌표를 곱하지 않아도 되고
// 글자는 distance field로 렌더되어 배율이 바뀌어도 뭉개지지 않습니다.
Item {
    id: root

    // ── C++이 채우는 값 ────────────────────────────────────────────────
    /** CCTV가 실제로 붙어 있는 구역 수. 나머지 자리는 확장용으로 비워 둡니다 */
    property int zoneCount: 2
    /** 객체 목록. 원소 하나가 MapObjectItem이 읽는 평평한 배열입니다 */
    property var mapObjects: []
    /** 채널별 위험 단계 (0=정상, 1=주의, 2=위험). 구역 순서로 CH01~CH04씩 이어 붙입니다 */
    property var channelRisk: []
    /** 채널별 LED 상태 (0=꺼짐, 1=안전, 2=주의, 3=위험) */
    property var channelLed: []
    /** 채널별 통합 알림 상태 (0=꺼짐, 1=대기, 2=동작) */
    property var channelAlert: []
    property bool dangerActive: false
    property bool showCctv: true
    property bool showLed: true
    property bool showAlertDevice: true
    property bool showMovementTrails: true

    /** C++이 월드 좌표를 옮길 때 쓰는 구역별 정사각 영역. [x,y,w,h] 를 구역 수만큼 이어 붙입니다 */
    readonly property var objectAreas: Plan.objectAreaList(root.zoneCount)

    signal zoneClicked(int zoneIndex)

    readonly property var plan: Plan.plan()
    readonly property real planScale: Math.min(width / plan.width, height / plan.height)

    readonly property var buildingOutline: [Qt.point(plan.building.x, plan.building.y),
                                            Qt.point(plan.building.x + plan.building.w, plan.building.y),
                                            Qt.point(plan.building.x + plan.building.w,
                                                     plan.building.y + plan.building.h),
                                            Qt.point(plan.building.x, plan.building.y + plan.building.h),
                                            Qt.point(plan.building.x, plan.building.y)]

    /** 평평한 [x,y,x,y,...] 좌표를 Shape가 받는 점 배열로 바꿉니다. */
    function toPoints(flat) {
        var points = [];
        for (var i = 0; i + 1 < flat.length; i += 2) {
            points.push(Qt.point(flat[i], flat[i + 1]));
        }
        return points;
    }

    /** 구역 하나의 채널 네 칸만 잘라 냅니다. 값이 아직 안 왔으면 빈 배열입니다 */
    function channelSlice(source, zoneIndex) {
        var start = zoneIndex * 4;
        if (!source || source.length < start + 4) {
            return [];
        }
        return [source[start], source[start + 1], source[start + 2], source[start + 3]];
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.mapBackdrop
    }

    Item {
        id: stage

        width: root.plan.width
        height: root.plan.height
        transformOrigin: Item.TopLeft
        scale: root.planScale
        x: (root.width - root.plan.width * root.planScale) / 2
        y: (root.height - root.plan.height * root.planScale) / 2

        // ── 바닥과 외벽 ────────────────────────────────────────────────
        // 아스팔트는 단색이 아니라 가운데가 아주 조금 밝은 원형 기울기입니다. 완전한 단색은
        // 넓은 면적에서 인쇄물처럼 납작해 보이는데, 이 한 겹이 조명 아래의 바닥처럼 읽히게 합니다.
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                strokeColor: "transparent"
                startX: root.plan.building.x
                startY: root.plan.building.y

                fillGradient: RadialGradient {
                    centerX: root.plan.building.x + root.plan.building.w / 2
                    centerY: root.plan.building.y + root.plan.building.h / 2
                    centerRadius: root.plan.building.w * 0.62
                    focalX: centerX
                    focalY: centerY

                    GradientStop {
                        position: 0.0
                        color: Theme.mapFloorLit
                    }
                    GradientStop {
                        position: 0.65
                        color: Theme.mapFloor
                    }
                    GradientStop {
                        position: 1.0
                        color: Theme.mapFloorEdge
                    }
                }

                PathPolyline {
                    path: root.buildingOutline
                }
            }
        }

        PlanWalls {
            paths: [root.buildingOutline]
            barWidth: 4.6
            shadowDepth: 2.4
            relief: 0.85
        }

        // ── 진출입 동선 ────────────────────────────────────────────────
        // 아래 차로가 그대로 진출입로입니다. 왼쪽은 설비실을 비워 둔 자리로 1층까지 곧게 나가고,
        // 오른쪽은 아래 모서리에서 곡선 램프로 꺾여 B2F로 내려갑니다.
        PlanWalls {
            paths: [root.toPoints(root.plan.ramp.outerWall), root.toPoints(root.plan.ramp.innerWall)]
            barWidth: 3.6
            relief: 0.8
        }

        // 램프 중심선
        Shape {
            anchors.fill: parent
            opacity: 0.8
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                strokeColor: Theme.mapPaper
                strokeWidth: 1.4
                strokeStyle: ShapePath.DashLine
                dashPattern: [5, 5]
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap

                PathPolyline {
                    path: root.toPoints(root.plan.ramp.centreLine)
                }
            }
        }

        // 진행 방향 화살표
        Repeater {
            model: root.plan.arrows

            Shape {
                id: arrowShape

                required property var modelData

                // 화살표는 자루 사각형과 머리 삼각형을 한 폴리곤으로 잇습니다.
                // sx/sy는 진행 방향에 수직인 축이라 좌우 대칭으로 폭을 잡을 수 있습니다.
                readonly property real sx: -arrowShape.modelData.dy
                readonly property real sy: arrowShape.modelData.dx
                readonly property real neckX: arrowShape.modelData.tipX - arrowShape.modelData.dx * 20
                readonly property real neckY: arrowShape.modelData.tipY - arrowShape.modelData.dy * 20
                readonly property real tailX: arrowShape.modelData.tipX
                                              - arrowShape.modelData.dx * arrowShape.modelData.len
                readonly property real tailY: arrowShape.modelData.tipY
                                              - arrowShape.modelData.dy * arrowShape.modelData.len

                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer

                ShapePath {
                    strokeColor: "transparent"
                    fillColor: Theme.mapPaper
                    startX: arrowShape.tailX + arrowShape.sx * 3
                    startY: arrowShape.tailY + arrowShape.sy * 3

                    PathLine {
                        x: arrowShape.neckX + arrowShape.sx * 3
                        y: arrowShape.neckY + arrowShape.sy * 3
                    }
                    PathLine {
                        x: arrowShape.neckX + arrowShape.sx * 10
                        y: arrowShape.neckY + arrowShape.sy * 10
                    }
                    PathLine {
                        x: arrowShape.modelData.tipX
                        y: arrowShape.modelData.tipY
                    }
                    PathLine {
                        x: arrowShape.neckX - arrowShape.sx * 10
                        y: arrowShape.neckY - arrowShape.sy * 10
                    }
                    PathLine {
                        x: arrowShape.neckX - arrowShape.sx * 3
                        y: arrowShape.neckY - arrowShape.sy * 3
                    }
                    PathLine {
                        x: arrowShape.tailX - arrowShape.sx * 3
                        y: arrowShape.tailY - arrowShape.sy * 3
                    }
                }
            }
        }

        Text {
            x: root.plan.exitLabelX
            y: root.plan.laneY - height / 2
            text: "1층"
            color: Theme.mapPaper
            font.family: Theme.fontFamily
            font.pixelSize: 15
            font.weight: Font.Bold
        }

        Text {
            x: root.plan.rampLabelX - width
            y: root.plan.laneY - height / 2 - 20
            text: "B2F"
            color: Theme.mapPaper
            font.family: Theme.fontFamily
            font.pixelSize: 15
            font.weight: Font.Bold
        }

        // ── 주차 구획 ──────────────────────────────────────────────────
        // 구획은 저마다 독립된 상자입니다. 피치 안에서 좌우·상하로 조금 물려 두어 옆 구획과
        // 바 사이에 어두운 틈이 남는데, 이 틈이 있어야 도면처럼 한 칸씩 떨어져 보입니다.
        // 속은 채우지 않습니다 — 실제 주차장 바닥은 구획 안팎이 같은 아스팔트입니다.
        // 구획선은 **바닥에 칠한 도색**입니다. 벽과 같은 입체를 주면 낮은 턱처럼 솟아 보여서
        // 실제 주차장과 전혀 달라집니다. 그림자도 두께도 없고 아스팔트와 닿는 얇은 경계만 남깁니다
        PlanWalls {
            paths: stage.stallOutlines
            barWidth: 3.2
            relief: 0.0
        }

        readonly property var stallOutlines: {
            var paths = [];
            var inset = 2.5;
            for (var i = 0; i < root.plan.stalls.length; ++i) {
                var s = root.plan.stalls[i];
                var l = s.x + inset;
                var t = s.y + inset;
                var r = s.x + s.w - inset;
                var b = s.y + s.h - inset;
                paths.push([Qt.point(l, t), Qt.point(r, t), Qt.point(r, b), Qt.point(l, b), Qt.point(l, t)]);
            }
            return paths;
        }

        // 차량 스토퍼. 차로 반대쪽 끝, 구획 하나에 하나씩 놓입니다
        Repeater {
            model: root.plan.stalls

            PlanWheelStop {
                required property var modelData

                barLength: modelData.w * 0.72
                x: modelData.x + (modelData.w - barLength) / 2
                y: modelData.stopAtTop ? modelData.y + 11 : modelData.y + modelData.h - 16
            }
        }

        // 기둥. 실제 도면에서는 속을 채운 어두운 사각형이고 주차열 경계선 위에 일정 간격으로 섭니다
        Repeater {
            model: root.plan.columns

            Item {
                id: column

                required property var modelData

                Rectangle {
                    x: column.modelData.x - 4.5 + 1.0
                    y: column.modelData.y - 4.5 + 1.6
                    width: 9
                    height: 9
                    radius: 1.2
                    color: Theme.mapWallShadow
                    opacity: 0.55
                }

                Rectangle {
                    x: column.modelData.x - 4.5
                    y: column.modelData.y - 4.5
                    width: 9
                    height: 9
                    radius: 1.2
                    border.width: 0.7
                    border.color: Theme.mapFloorEdge
                    gradient: Gradient {
                        GradientStop {
                            position: 0.0
                            color: Theme.mapColumnLit
                        }
                        GradientStop {
                            position: 1.0
                            color: Theme.mapColumn
                        }
                    }
                }
            }
        }

        // ── 설비실 복도 벽 ─────────────────────────────────────────────
        PlanWalls {
            paths: stage.wallLines
            barWidth: 3.6
            relief: 0.8
        }

        readonly property var wallLines: {
            var paths = [];
            for (var i = 0; i < root.plan.walls.length; ++i) {
                var w = root.plan.walls[i];
                paths.push([Qt.point(w.x1, w.y1), Qt.point(w.x2, w.y2)]);
            }
            return paths;
        }

        // ── 설비실과 승강기 코어 ───────────────────────────────────────
        Repeater {
            model: root.plan.rooms

            PlanRoom {
                required property var modelData

                room: modelData
            }
        }

        Repeater {
            model: root.plan.cores

            PlanRoom {
                required property var modelData

                room: modelData
                labelSize: 8.5
            }
        }

        // ── 구역과 채널 ────────────────────────────────────────────────
        Repeater {
            model: root.plan.zones

            ZoneStation {
                required property var modelData
                required property int index

                zone: modelData
                zoneIndex: index
                active: index < root.zoneCount
                channelRisk: root.channelSlice(root.channelRisk, index)
                channelLed: root.channelSlice(root.channelLed, index)
                channelAlert: root.channelSlice(root.channelAlert, index)
                showCctv: root.showCctv
                showLed: root.showLed
                showAlertDevice: root.showAlertDevice
                onClicked: root.zoneClicked(index)
            }
        }

        Repeater {
            model: root.plan.zones

            Text {
                required property var modelData
                required property int index

                visible: index >= root.zoneCount
                x: modelData.cx - width / 2
                y: modelData.cy - height / 2
                text: "확장 예정"
                color: Theme.mapPaper
                opacity: 0.35
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
        }

        // ── 실시간 객체 ────────────────────────────────────────────────
        Repeater {
            model: root.mapObjects

            MapObjectItem {
                required property var modelData

                fields: modelData
                showTrail: root.showMovementTrails
            }
        }
    }

    // ── 위험 테두리 ────────────────────────────────────────────────────
    // 바깥에서 안으로 굵고 흐린 겹이 번지는 빛이 되고, 안쪽으로 갈수록 좁고 진해집니다.
    // 색은 네 겹 모두 순수한 빨강 쪽입니다 — 밝은 분홍 심지를 넣으면 경고가 아니라
    // 조명처럼 보여서, 위험 신호로 읽히는 것은 결국 붉기입니다.
    Item {
        id: dangerBorder

        anchors.fill: parent
        // visible을 dangerActive에 직접 걸면 꺼지는 순간 그대로 사라져 **페이드 아웃이 아예
        // 재생되지 않습니다.** 불투명도가 다 빠질 때까지 보이게 두어야 합니다
        visible: dangerBorder.opacity > 0.005
        opacity: root.dangerActive ? 1.0 : 0.0

        Behavior on opacity {
            NumberAnimation {
                duration: 520
                easing.type: Easing.InOutQuad
            }
        }

        Repeater {
            model: [
                {"w": 13, "min": 0.0, "max": 0.30, "c": "#ff0a1e"},
                {"w": 7, "min": 0.0, "max": 0.48, "c": "#f5081c"},
                {"w": 3.4, "min": 0.06, "max": 0.78, "c": "#ff1626"},
                {"w": 1.4, "min": 0.12, "max": 1.0, "c": "#ff2f3d"}
            ]

            Rectangle {
                id: stroke

                required property var modelData

                anchors.fill: parent
                anchors.margins: modelData.w / 2
                color: "transparent"
                border.width: modelData.w
                border.color: modelData.c
                opacity: modelData.min

                SequentialAnimation on opacity {
                    running: root.dangerActive
                    loops: Animation.Infinite

                    NumberAnimation {
                        to: stroke.modelData.max
                        duration: 760
                        easing.type: Easing.InOutSine
                    }
                    NumberAnimation {
                        to: stroke.modelData.min
                        duration: 1040
                        easing.type: Easing.InOutSine
                    }
                }
            }
        }
    }
}
