import QtQuick
import "Theme.js" as Theme

Item {
    id: root

    required property string title
    required property string status
    required property color statusColor

    width: Math.max(184, statusRow.implicitWidth + 32)
    height: 62

    Row {
        id: statusRow

        anchors.centerIn: parent
        spacing: 8

        Text {
            text: root.title
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 17
            font.weight: Font.Bold
            font.letterSpacing: 0
        }

        Text {
            text: root.status
            color: root.statusColor
            font.family: Theme.fontFamily
            font.pixelSize: 17
            font.weight: Font.Bold
            font.letterSpacing: 0
        }
    }
}
