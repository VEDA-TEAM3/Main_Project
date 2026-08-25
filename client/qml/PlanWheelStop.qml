pragma ComponentBehavior: Bound

import QtQuick
import "Theme.js" as Theme

// 주차 구획 하나에 놓인 차량 스토퍼.
//
// **한 구획에 하나입니다.** 앞바퀴 자리마다 짧은 토막을 둘 두면, 줄을 따라 짧은 노란 조각이
// 일정한 간격으로 반복되어 눈에는 그냥 파선으로 읽힙니다. 조각이 작을수록 검정 테와 노란 면이
// 화면에서 뭉개져 내부 구조가 사라지고, 남는 것은 노란 점선뿐입니다.
//
// 그래서 구획 폭의 7할을 차지하는 긴 몸통 하나로 두고, 그 안에서 노랑과 검정을 나눕니다.
// 구획마다 물건이 하나씩 놓인 것으로 읽히고, 실제 경고 도색된 스토퍼의 생김새이기도 합니다.
//
//   [검정 끝] [노랑] [검정 띠] [노랑] [검정 끝]
//
// 아래와 양 끝에 검정을 남기는 것은 색 대비를 위해서가 아니라, 그것이 고무 몸통이고 노란 면은
// 그 위에 얹힌 경고 도색이기 때문입니다.
Item {
    id: root

    /** 스토퍼 전체 길이 */
    required property real barLength
    property real barHeight: 7.0

    /// 양 끝에 남기는 검정 고무
    readonly property real capWidth: barLength * 0.13
    /// 가운데를 가르는 검정 띠. 이것 하나로 긴 막대가 아니라 경고 도색된 물건으로 읽힙니다
    readonly property real bandWidth: barLength * 0.12
    /// 아래에 남기는 검정 앞면. 위에서 비스듬히 본 물건의 옆면이다
    readonly property real faceDepth: barHeight * 0.30
    /// 위에 남기는 검정 뒷면
    readonly property real backDepth: barHeight * 0.13

    readonly property real faceWidth: (barLength - capWidth * 2 - bandWidth) / 2

    width: barLength
    height: barHeight

    // 바닥 그림자. 벽·설비와 같은 방향(왼쪽 위 광원)이라야 따로 놀지 않습니다
    Rectangle {
        x: 0.9
        y: 1.6
        width: root.barLength
        height: root.barHeight
        radius: 1.8
        color: Theme.mapWallShadow
        opacity: 0.55
    }

    // 검정 고무 몸통.
    //
    // 윤곽선이 있어야 합니다. 검정 몸통은 아스팔트와 밝기가 비슷해 그냥 두면 양 끝이 바닥에
    // 묻히고, 결국 가운데 노란 두 칸만 남아 다시 파선으로 보입니다. 실물도 고무 모서리가
    // 빛을 받아 테두리가 살아 있습니다
    Rectangle {
        anchors.fill: parent
        radius: 1.8
        color: Theme.mapWheelStopDark
        border.width: 0.7
        border.color: Theme.mapWheelStopEdge
    }

    // 노란 경고 도색 두 칸
    Repeater {
        model: [root.capWidth, root.capWidth + root.faceWidth + root.bandWidth]

        Item {
            id: face

            required property real modelData

            Rectangle {
                x: face.modelData
                y: root.backDepth
                width: root.faceWidth
                height: root.barHeight - root.faceDepth - root.backDepth
                radius: 0.8

                gradient: Gradient {
                    GradientStop {
                        position: 0.0
                        color: Theme.mapWheelStopLit
                    }
                    GradientStop {
                        position: 1.0
                        color: Theme.mapWheelStop
                    }
                }
            }

            // 윗면 모서리에 걸리는 빛
            Rectangle {
                x: face.modelData + 0.6
                y: root.backDepth + 0.5
                width: root.faceWidth - 1.2
                height: 0.9
                radius: 0.45
                color: Theme.mapPaper
                opacity: 0.38
            }
        }
    }
}
