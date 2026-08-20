import QtQuick
import "Theme.js" as Theme

PanelFrame {
    id: root

    implicitHeight: 46
    color: Theme.surface

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 18
        anchors.verticalCenter: parent.verticalCenter
        spacing: 18

        Text {
            text: "Status Legend"
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            font.letterSpacing: 0
        }

        Rectangle { width: 1; height: 18; color: Theme.border }
        LegendItem { statusText: "● NORMAL"; description: "Safe"; statusColor: Theme.safe }
        Rectangle { width: 1; height: 18; color: Theme.border }
        LegendItem { statusText: "● WARNING"; description: "Caution"; statusColor: Theme.warning }
        Rectangle { width: 1; height: 18; color: Theme.border }
        LegendItem { statusText: "● DANGER"; description: "Act now"; statusColor: Theme.danger }
    }

    component LegendItem: Row {
        required property string statusText
        required property string description
        required property color statusColor
        spacing: 8

        Text {
            text: parent.statusText
            color: parent.statusColor
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.Bold
            font.letterSpacing: 0
        }

        Text {
            text: parent.description
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            font.letterSpacing: 0
        }
    }
}
