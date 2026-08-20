pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import "Theme.js" as Theme

// 도면 문 기호 하나. 열린 자리의 문짝 한 획과 닫힌 자리까지 도는 호로 그립니다.
// 벽 개구부는 이 항목이 아니라 PlanRoom이 벽 위에 바닥색 조각을 덮어 냅니다.
Shape {
    id: root

    /** ParkingPlan.js의 door() 결과 */
    required property var door

    anchors.fill: parent
    preferredRendererType: Shape.CurveRenderer
    opacity: 0.85

    ShapePath {
        strokeColor: Theme.mapRoomInk
        strokeWidth: 1.1
        fillColor: "transparent"
        capStyle: ShapePath.RoundCap
        startX: root.door.hx
        startY: root.door.hy

        PathLine {
            x: root.door.leafX
            y: root.door.leafY
        }

        PathArc {
            x: root.door.endX
            y: root.door.endY
            radiusX: root.door.r
            radiusY: root.door.r
            direction: root.door.clockwise ? PathArc.Clockwise : PathArc.Counterclockwise
        }
    }
}
