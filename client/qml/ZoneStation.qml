pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import "Theme.js" as Theme

// 물리 CCTV 한 대가 담당하는 구역. 카메라는 가운데 서서 위·오른쪽·아래·왼쪽 네 방향을 보므로
// 채널 경계는 구역 사각형의 두 대각선이고, 채널 하나는 그 사분면 삼각형입니다.
//
// 위험한 채널은 그 삼각형을 옅게 칠하고 가장자리를 진하게 두릅니다. 깜빡이지 않습니다.
Item {
    id: root

    /** ParkingPlan.js의 zone 하나 */
    required property var zone
    required property int zoneIndex
    /** 이 구역 네 채널의 위험 단계 (0=정상, 1=주의, 2=위험) */
    property var channelRisk: []
    /** 채널별 LED 상태 (0=꺼짐, 1=안전, 2=주의, 3=위험) */
    property var channelLed: []
    /** 채널별 통합 알림 상태 (0=꺼짐, 1=대기, 2=동작) */
    property var channelAlert: []
    property bool active: true
    property bool showCctv: true
    property bool showLed: true
    property bool showAlertDevice: true

    signal clicked

    readonly property real iconSize: 26
    readonly property real iconGap: 5

    x: zone.x
    y: zone.y
    width: zone.w
    height: zone.h

    function riskColor(level) {
        return level >= 2 ? Theme.danger : Theme.warning;
    }

    // ── 채널 위험 표시 ─────────────────────────────────────────────────
    // 0=위, 1=오른쪽, 2=아래, 3=왼쪽. 카메라가 구역 한가운데서 네 방향을 보므로 채널 하나가
    // 맡는 자리는 두 대각선이 자르는 사분면 삼각형입니다.
    //
    // 그 **면 전체를 한 가지 색으로 채웁니다.** 예전에는 카메라에서 바깥으로 옅어지는 부채꼴로
    // 깔았는데, 정작 사람이 서 있는 구역 가장자리가 거의 투명해져서 위험이 눈에 띄지 않았습니다.
    // 관제 화면에서 이 표시는 은근할 이유가 없습니다 — 어느 채널이 위험한지 즉시 보여야 합니다.
    Repeater {
        model: 4

        Item {
            id: sector

            required property int index
            readonly property int level: root.channelRisk[sector.index] !== undefined
                                             ? root.channelRisk[sector.index] : 0
            readonly property var corners: [[0, 0], [root.width, 0], [root.width, root.height], [0, root.height]]
            readonly property var from: sector.corners[sector.index]
            readonly property var to: sector.corners[(sector.index + 1) % 4]

            // Item 자체의 불투명도는 나타나고 사라지는 데만 씁니다. 면과 테두리의 농도는
            // 색에 직접 실어야 둘의 대비가 유지됩니다 — 여기서 한꺼번에 눌러 버리면
            // 테두리가 면과 같은 농도가 되어 사라집니다
            anchors.fill: parent
            opacity: sector.level > 0 ? 1.0 : 0.0
            visible: opacity > 0.01

            Behavior on opacity {
                NumberAnimation {
                    duration: 220
                    easing.type: Easing.OutQuad
                }
            }

            // **깜빡이지 않습니다.** 움직이는 표시는 눈이 금방 지치고, 정작 그 위를 지나가는
            // 객체를 놓칩니다. 대신 면을 통째로 칠하고 가장자리까지 진하게 둘러, 도로 표지처럼
            // "여기가 지정된 경고 구역"으로 한 번에 읽히게 합니다.
            //
            // 면과 테두리는 **둘 다** 필요합니다. 테두리만 두르면 어느 채널이 위험한지 선을
            // 따라가며 읽어야 하고, 면만 칠하면 경계가 흐려집니다
            // **CurveRenderer를 쓰지 않습니다.** 저 렌더러는 면 색을 정점에 구워 두고
            // 도형을 다시 만들 때만 갱신하므로, 주의에서 위험으로 바뀌면 테두리만 빨개지고
            // 면은 노란색으로 남습니다. 삼각형에는 곡선이 없어 얻을 것도 없습니다
            Shape {
                anchors.fill: parent

                ShapePath {
                    strokeColor: Qt.alpha(root.riskColor(sector.level), sector.level >= 2 ? 1.0 : 0.8)
                    strokeWidth: sector.level >= 2 ? 2.4 : 1.5
                    fillColor: Qt.alpha(root.riskColor(sector.level), sector.level >= 2 ? 0.55 : 0.38)
                    joinStyle: ShapePath.MiterJoin
                    startX: root.width / 2
                    startY: root.height / 2

                    PathLine {
                        x: sector.from[0]
                        y: sector.from[1]
                    }
                    PathLine {
                        x: sector.to[0]
                        y: sector.to[1]
                    }
                    // 획을 닫아야 두 대각선까지 테두리가 이어집니다. 열어 두면 바깥변에만 선이 남습니다
                    PathLine {
                        x: root.width / 2
                        y: root.height / 2
                    }
                }
            }
        }
    }

    // ── 비활성 구역 가리개 ─────────────────────────────────────────────
    // CCTV가 아직 없는 자리는 도면째로 어둡게 덮습니다. 테두리 색만 다르게 두는 것보다
    // 밝기 차이가 훨씬 빨리 읽혀서, 지금 실제로 보고 있는 구역이 어디인지 한눈에 들어옵니다.
    Rectangle {
        x: root.zone.maskX - root.zone.x
        y: root.zone.maskY - root.zone.y
        width: root.zone.maskW
        height: root.zone.maskH
        visible: !root.active
        color: Theme.mapZoneMasked
        opacity: 0.5
    }

    // ── 채널 경계 ──────────────────────────────────────────────────────
    // 구역 테두리는 긋지 않습니다. 도면에 이미 벽·구획선이 촘촘한데 그 위에 사각형을 하나 더
    // 얹으면 선이 겹쳐 어느 것이 건물이고 어느 것이 표기인지 흐려집니다. 구역의 범위는
    // 대각선이 뻗어 나가는 방향과, 비활성 자리를 통째로 덮는 가리개의 밝기 차이로 읽힙니다.
    //
    // 아직 CCTV가 없는 자리에는 대각선도 긋지 않습니다.
    Shape {
        anchors.fill: parent
        visible: root.active
        opacity: 0.5
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: Theme.mapPaper
            strokeWidth: 1.0
            strokeStyle: ShapePath.DashLine
            dashPattern: [4, 6]
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            startX: 0
            startY: 0

            PathLine {
                x: root.width
                y: root.height
            }
            PathMove {
                x: root.width
                y: 0
            }
            PathLine {
                x: 0
                y: root.height
            }
        }
    }

    // ── 카메라 ─────────────────────────────────────────────────────────
    MapCamera {
        extent: 34
        x: root.width / 2 - width / 2
        y: root.height / 2 - height / 2
        visible: root.showCctv && root.active
    }

    // ── 채널별 이름표와 장치 상태 ──────────────────────────────────────
    // 배경 판은 깔지 않습니다. 구획은 속을 비워 두므로 이 자리는 거의 다 어두운 바닥이고,
    // 판을 깔면 그 아래의 구획선과 지나가는 객체가 사라집니다.
    Repeater {
        model: root.active ? 4 : 0

        Item {
            id: chip

            required property int index
            readonly property var spots: [[root.width / 2, 36], [root.width - 44, root.height / 2],
                                          [root.width / 2, root.height - 36], [44, root.height / 2]]

            x: chip.spots[chip.index][0] - width / 2
            y: chip.spots[chip.index][1] - height / 2
            width: root.iconSize * 2 + root.iconGap
            height: 42

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 0
                text: "CH" + (chip.index + 1 < 10 ? "0" : "") + (chip.index + 1)
                color: Theme.mapPaper
                opacity: 0.95
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.DemiBold
            }

            Image {
                x: 0
                y: parent.height - root.iconSize
                width: root.iconSize
                height: root.iconSize
                visible: root.showLed
                mipmap: true
                source: {
                    var state = root.channelLed[chip.index] !== undefined ? root.channelLed[chip.index] : 0;
                    if (state === 3)
                        return "qrc:/icons/led_danger.png";
                    if (state === 2)
                        return "qrc:/icons/led_waring.png";
                    if (state === 1)
                        return "qrc:/icons/led_safe.png";
                    return "qrc:/icons/led_off.png";
                }
                sourceSize: Qt.size(64, 64)
                fillMode: Image.PreserveAspectFit
                smooth: true
            }

            Image {
                x: parent.width - root.iconSize
                y: parent.height - root.iconSize
                width: root.iconSize
                height: root.iconSize
                visible: root.showAlertDevice
                mipmap: true
                source: {
                    var state = root.channelAlert[chip.index] !== undefined ? root.channelAlert[chip.index] : 0;
                    if (state === 2)
                        return "qrc:/icons/sensor_danger.png";
                    if (state === 1)
                        return "qrc:/icons/sensor_safe.png";
                    return "qrc:/icons/sensor_off.png";
                }
                sourceSize: Qt.size(64, 64)
                fillMode: Image.PreserveAspectFit
                smooth: true
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.active
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
