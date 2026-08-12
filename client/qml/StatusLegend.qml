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
            text: "상태 색상 범례"
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            font.letterSpacing: 0
        }

        Rectangle { width: 1; height: 18; color: Theme.border }
        LegendItem { statusText: "● 정상"; description: ": 안전 상태"; statusColor: Theme.safe }
        Rectangle { width: 1; height: 18; color: Theme.border }
        LegendItem { statusText: "● 주의"; description: ": 유의 필요"; statusColor: Theme.warning }
        Rectangle { width: 1; height: 18; color: Theme.border }
        LegendItem { statusText: "● 위험"; description: ": 즉시 대응 필요"; statusColor: Theme.danger }
    }

    component LegendItem: Row {
        required property string statusText
        required property string description
        required property color statusColor
        spacing: 5

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
