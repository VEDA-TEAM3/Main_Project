pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import "Theme.js" as Theme

// CCTV 도면 기호. 한 대가 네 방향(위·오른쪽·아래·왼쪽)을 보는 4채널 카메라입니다.
//
// 한 방향만 보는 옆모습으로 그리면 안 됩니다 — 이 장비는 구역 한가운데 서서 사분면 넷을
// 나눠 맡고, 채널 경계 대각선도 거기서 뻗어 나갑니다. 기호가 한쪽만 가리키면 나머지 세 채널이
// 어디서 나온 것인지 도면에서 읽히지 않습니다.
//
// 금속 질감을 낸 돔 대신 납작한 도면 기호로 그립니다. 지도가 얇은 흰 선과 회색 면으로만 되어
// 있어서, 혼자 입체로 렌더된 물건이 있으면 그것부터 눈에 들어와 정작 봐야 할 위험 표시와
// 객체가 뒤로 밀립니다. 속을 어둡게 채우고 선만 흰색이라 노랑·빨강 위험 면 위에서도 읽힙니다.
//
// 좌표는 0~1 비율로 두고 extent로 한 번에 키웁니다. 배율이 바뀌어도 선 굵기 비례가 유지됩니다.
Item {
    id: root

    /** 기호가 차지하는 한 변(도면 단위) */
    property real extent: 40

    readonly property real c: extent / 2

    implicitWidth: extent
    implicitHeight: extent
    width: extent
    height: extent

    function px(ratio) {
        return ratio * root.extent;
    }

    // 방향별 카메라 하나. 위를 보는 모양 하나를 90도씩 돌려 넷을 만듭니다
    Repeater {
        model: 4

        Shape {
            id: unit

            required property int index

            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            transform: Rotation {
                origin.x: root.c
                origin.y: root.c
                angle: unit.index * 90
            }

            // 몸체와 앞으로 좁아지는 렌즈를 한 윤곽으로 잇습니다. 따로 그리면 맞닿는 자리에
            // 선이 겹쳐 접합부가 도드라집니다. 끝으로 갈수록 좁아져야 그 방향을 본다는 것이
            // 읽힙니다 — 벌어지게 두면 넷이 모여 십자 무늬로만 보입니다
            ShapePath {
                strokeColor: Theme.mapPaper
                strokeWidth: root.px(0.038)
                fillColor: Theme.mapCameraBody
                joinStyle: ShapePath.MiterJoin
                startX: root.c - root.px(0.085)
                startY: root.c - root.px(0.12)

                PathLine {
                    x: root.c - root.px(0.085)
                    y: root.c - root.px(0.30)
                }
                PathLine {
                    x: root.c - root.px(0.050)
                    y: root.c - root.px(0.42)
                }
                PathLine {
                    x: root.c + root.px(0.050)
                    y: root.c - root.px(0.42)
                }
                PathLine {
                    x: root.c + root.px(0.085)
                    y: root.c - root.px(0.30)
                }
                PathLine {
                    x: root.c + root.px(0.085)
                    y: root.c - root.px(0.12)
                }
                PathLine {
                    x: root.c - root.px(0.085)
                    y: root.c - root.px(0.12)
                }
            }

            // 렌즈
            ShapePath {
                strokeColor: Theme.mapPaper
                strokeWidth: root.px(0.028)
                fillColor: Theme.mapCameraLens

                PathAngleArc {
                    centerX: root.c
                    centerY: root.c - root.px(0.345)
                    radiusX: root.px(0.036)
                    radiusY: root.px(0.036)
                    startAngle: 0
                    sweepAngle: 360
                }
            }
        }
    }

    // 가운데 고정 기둥. 네 몸체의 안쪽 끝을 덮어 하나의 장비로 묶습니다
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: Theme.mapPaper
            strokeWidth: root.px(0.042)
            fillColor: Theme.mapCameraBody

            PathAngleArc {
                centerX: root.c
                centerY: root.c
                radiusX: root.px(0.135)
                radiusY: root.px(0.135)
                startAngle: 0
                sweepAngle: 360
            }
        }
    }
}
