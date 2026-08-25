pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import "Theme.js" as Theme

/** 설정창에서 쓰는 선택 상자입니다. 화살표까지 테마에 맞춰 직접 그립니다. */
ComboBox {
    id: root

    implicitWidth: 190
    implicitHeight: 34
    hoverEnabled: true

    indicator: Canvas {
        x: root.width - width - 12
        y: (root.height - height) / 2
        width: 12
        height: 8
        onPaint: {
            const context = getContext("2d")
            context.reset()
            context.strokeStyle = root.enabled ? Theme.borderStrong : Theme.textMuted
            context.lineWidth = 1.8
            context.lineCap = "round"
            context.lineJoin = "round"
            context.beginPath()
            context.moveTo(1, 2)
            context.lineTo(width / 2, height - 2)
            context.lineTo(width - 1, 2)
            context.stroke()
        }
    }

    contentItem: Text {
        leftPadding: 12
        rightPadding: root.indicator.width + 18
        text: root.displayText
        color: root.enabled ? Theme.text : Theme.textMuted
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 14
        font.weight: Font.DemiBold
        font.letterSpacing: 0
    }

    background: Rectangle {
        radius: 4
        color: root.enabled ? Theme.background : Theme.surface
        border.width: 1
        border.color: root.hovered && root.enabled ? Theme.borderStrong : Theme.border

        Behavior on border.color { ColorAnimation { duration: 110 } }
    }

    popup: Popup {
        y: root.height + 2
        width: root.width
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 220)
        padding: 4

        background: Rectangle {
            radius: 4
            color: Theme.surfaceRaised
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.delegateModel
            currentIndex: root.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
        }
    }

    delegate: ItemDelegate {
        id: option

        required property int index
        required property var modelData
        width: ListView.view.width
        height: 32
        hoverEnabled: true

        contentItem: Text {
            leftPadding: 8
            text: option.modelData
            color: Theme.text
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            font.letterSpacing: 0
        }

        background: Rectangle {
            radius: 3
            color: option.hovered || root.currentIndex === option.index ? Theme.cyanDark : "transparent"
        }
    }

    HoverHandler { cursorShape: Qt.PointingHandCursor }
}
