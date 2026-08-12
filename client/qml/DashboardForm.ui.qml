import QtQuick
import "Theme.js" as Theme

Rectangle {
    width: 1920
    height: 1080
    color: Theme.background

    Column {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 12

        TopBarForm { width: parent.width }

        Row {
            width: parent.width
            height: 520
            spacing: 12

            PanelFrame {
                width: (parent.width - 12) * 0.52
                height: parent.height
                CctvToolbar {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    anchors.topMargin: 8
                }
            }

            PanelFrame {
                width: (parent.width - 12) * 0.48
                height: parent.height
                PanelHeader {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    anchors.topMargin: 8
                    titleText: "디지털 트윈 2D 맵"
                }
            }
        }

        Row {
            width: parent.width
            height: 380
            spacing: 12

            DashboardPanel { titleText: "실시간 객체 목록" }
            DashboardPanel { titleText: "이벤트 로그" }
            DashboardPanel { titleText: "장비 제어 / 상태" }
        }

        StatusLegend { width: parent.width }
    }

    component DashboardPanel: PanelFrame {
        required property string titleText
        width: (parent.width - 24) / 3
        height: parent.height

        PanelHeader {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.topMargin: 8
            titleText: parent.titleText
        }
    }
}
