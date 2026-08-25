import QtQuick
import QtQuick.Controls.Basic
import "Theme.js" as Theme

/** 설정창에서 쓰는 체크 항목입니다. 위젯 시절 QSS 모양을 그대로 옮겼습니다. */
CheckBox {
    id: root

    implicitHeight: 24
    padding: 0
    spacing: 10
    hoverEnabled: true

    indicator: Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        x: 0
        width: 20
        height: 20
        radius: 3
        color: root.checked ? Theme.cyanDark : Theme.background
        border.width: 1
        border.color: root.checked || root.hovered ? Theme.borderStrong : Theme.border

        Behavior on color { ColorAnimation { duration: 110 } }

        // 체크 표시는 두 선분을 직접 그려 플랫폼 기본 모양에 의존하지 않습니다.
        Canvas {
            anchors.fill: parent
            visible: root.checked
            onPaint: {
                const context = getContext("2d")
                context.reset()
                context.strokeStyle = Theme.text
                context.lineWidth = 2.4
                context.lineCap = "round"
                context.lineJoin = "round"
                context.beginPath()
                context.moveTo(width * 0.26, height * 0.52)
                context.lineTo(width * 0.44, height * 0.72)
                context.lineTo(width * 0.76, height * 0.30)
                context.stroke()
            }
        }
    }

    contentItem: Text {
        leftPadding: root.indicator.width + root.spacing
        text: root.text
        color: root.enabled ? Theme.text : Theme.textMuted
        verticalAlignment: Text.AlignVCenter
        font.family: Theme.fontFamily
        font.pixelSize: 14
        font.weight: Font.DemiBold
        font.letterSpacing: 0
    }

    background: Item {}

    HoverHandler { cursorShape: Qt.PointingHandCursor }
}
