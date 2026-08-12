import QtQuick
import "Theme.js" as Theme

Rectangle {
    id: root

    property alias text: label.text
    property url iconSource
    property bool selected: false
    /** 창을 열자마자 현재 선택으로 바로 보이도록, 여는 순간에는 전환을 끄고 스냅합니다. */
    property bool animated: true
    signal clicked

    implicitWidth: 92
    implicitHeight: 38
    radius: 5
    color: root.selected ? Theme.cyanDark : (hoverHandler.hovered ? Theme.surfaceRaised : Theme.surface)
    border.width: 1
    border.color: root.selected || hoverHandler.hovered ? Theme.borderStrong : Theme.border
    opacity: root.enabled ? 1.0 : 0.45
    scale: tapHandler.pressed ? 0.98 : 1.0

    Behavior on color { enabled: root.animated; ColorAnimation { duration: 120 } }
    Behavior on border.color { enabled: root.animated; ColorAnimation { duration: 120 } }
    Behavior on scale { NumberAnimation { duration: 70; easing.type: Easing.OutCubic } }

    Row {
        anchors.centerIn: parent
        spacing: icon.visible ? 6 : 0

        Image {
            id: icon
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            visible: root.iconSource.toString().length > 0
            source: root.iconSource
            fillMode: Image.PreserveAspectFit
            smooth: true
        }

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            font.letterSpacing: 0
        }
    }

    HoverHandler {
        id: hoverHandler
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
    }

    TapHandler {
        id: tapHandler
        enabled: root.enabled
        acceptedButtons: Qt.LeftButton
        onTapped: root.clicked()
    }
}
