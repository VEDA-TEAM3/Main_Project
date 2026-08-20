pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import "Theme.js" as Theme

// 도면의 벽·구획선 한 벌. 같은 path를 여러 겹으로 겹쳐 그어 두께 있는 구조물처럼 보이게 합니다.
//
//   1~2) 바닥 그림자 두 겹 — 멀고 옅은 겹 + 가깝고 진한 겹. 한 겹만 깔면 가장자리가 칼같이
//        끊겨 스티커처럼 보이는데, 두 겹이 겹치면 번지는 그림자처럼 읽힙니다
//     3) 어두운 테        — 밝은 면의 윤곽을 잡아 아스팔트와 경계를 만듭니다
//     4) 밝은 면          — 실제로 보이는 벽면
//     5) 오른쪽 아래 그늘 — 빛이 왼쪽 위에서 온다고 보고 반대쪽에 그늘을 넣습니다
//     6) 왼쪽 위 하이라이트 — 모서리가 깎여 빛을 받는 자리
//
// 벽·구획·램프·외벽이 전부 이 컴포넌트를 쓰므로 굵기와 빛 방향이 저절로 한 벌로 맞습니다.
// 방향을 바꾸려면 여기 한 곳만 고치면 도면 전체가 함께 따라옵니다.
Item {
    id: root

    /** 그릴 path 목록. 원소 하나가 Qt.point 배열입니다 */
    required property var paths
    /** 밝은 면의 굵기. 나머지 겹은 여기에 맞춰 두꺼워지거나 얇아집니다 */
    property real barWidth: 3.0
    property real shadowDepth: 1.0

    /**
     * 얼마나 서 있는 물건으로 보일지. 1이면 바닥에서 솟은 구조물, 0이면 바닥에 칠한 도색입니다.
     *
     * **주차 구획선은 0에 가깝게 두세요.** 벽과 같은 입체를 주면 구획선이 낮은 턱처럼 솟아 보여
     * 실제 주차장과 전혀 달라집니다. 도색은 두께도 그림자도 없고, 아스팔트와 닿는 자리에
     * 얇은 경계만 생깁니다. 벽·설비·램프처럼 정말로 서 있는 것에만 1에 가까운 값을 줍니다.
     */
    property real relief: 1.0

    anchors.fill: parent

    /// 입체가 줄면 어두운 테도 함께 얇아져야 합니다. 굵기가 그대로면 도색에 굵은 윤곽선이 남습니다
    readonly property real edgeWidth: root.barWidth + 1.9 * (0.3 + 0.7 * root.relief)

    readonly property var passes: [
        {"dx": 2.6 * root.shadowDepth, "dy": 3.4 * root.shadowDepth, "w": root.barWidth + 2.4,
         "c": Theme.mapWallShadow, "o": 0.26 * root.relief},
        {"dx": 1.2 * root.shadowDepth, "dy": 1.7 * root.shadowDepth, "w": root.barWidth + 1.9,
         "c": Theme.mapWallShadow, "o": 0.45 * root.relief},
        {"dx": 0, "dy": 0, "w": root.edgeWidth, "c": Theme.mapWallShade, "o": 1.0},
        {"dx": 0, "dy": 0, "w": root.barWidth, "c": Theme.mapWall, "o": 1.0},
        {"dx": 0.72, "dy": 0.82, "w": root.barWidth * 0.46, "c": Theme.mapWallMid, "o": 0.65 * root.relief},
        {"dx": -0.72, "dy": -0.82, "w": root.barWidth * 0.40, "c": Theme.mapWallLit, "o": 0.55 * root.relief}
    ]

    Repeater {
        model: root.passes

        Shape {
            id: pass

            required property var modelData

            anchors.fill: parent
            opacity: pass.modelData.o
            visible: pass.modelData.o > 0.01
            preferredRendererType: Shape.CurveRenderer
            transform: Translate {
                x: pass.modelData.dx
                y: pass.modelData.dy
            }

            ShapePath {
                strokeColor: pass.modelData.c
                strokeWidth: pass.modelData.w
                fillColor: "transparent"
                joinStyle: ShapePath.MiterJoin

                PathMultiline {
                    paths: root.paths
                }
            }
        }
    }
}
