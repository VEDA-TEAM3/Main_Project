import QtQuick
import QtQuick.Controls.Basic
import "Theme.js" as Theme

/** 영상 보정용 정수 슬라이더입니다. 값 표시는 쓰는 쪽에서 따로 붙입니다. */
Slider {
    id: root

    implicitHeight: 24
    stepSize: 1
    snapMode: Slider.SnapAlways

    background: Rectangle {
        x: root.leftPadding
        y: root.topPadding + (root.availableHeight - height) / 2
        width: root.availableWidth
        height: 5
        radius: 2.5
        color: Theme.background
        border.width: 1
        border.color: Theme.border

        Rectangle {
            width: root.visualPosition * parent.width
            height: parent.height
            radius: parent.radius
            color: root.enabled ? Theme.cyan : Theme.textMuted
        }
    }

    handle: Rectangle {
        x: root.leftPadding + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + (root.availableHeight - height) / 2
        width: 16
        height: 16
        radius: 8
        color: root.pressed ? Theme.cyan : Theme.surfaceRaised
        border.width: 2
        border.color: root.enabled ? Theme.borderStrong : Theme.border

        Behavior on color { ColorAnimation { duration: 90 } }
    }

    HoverHandler { cursorShape: Qt.PointingHandCursor }
}
