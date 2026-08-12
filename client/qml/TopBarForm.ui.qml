import QtQuick
import "Theme.js" as Theme

Item {
    id: root

    property string titleText: "<font color=\"#a8d4ff\">Wise AI</font> 기반 주차장 디지털 트윈 관제 시스템"
    property string areaText: "제 1구역"
    property string dateTimeText: ""
    property string systemStatusText: "● 연결 중"
    property color systemStatusColor: "#ff4b4b"
    property string cctvStatusText: "● 연결 중"
    property color cctvStatusColor: "#ff4b4b"

    property alias areaTapHandler: areaTapHandler
    property alias settingsTapHandler: settingsTapHandler

    implicitHeight: 62

    Rectangle {
        anchors.fill: parent
        radius: 6
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#174b69" }
            GradientStop { position: 0.38; color: Theme.surfaceRaised }
            GradientStop { position: 1.0; color: Theme.surface }
        }
        border.width: 1
        border.color: Theme.border

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            text: root.titleText
            color: Theme.text
            textFormat: Text.StyledText
            font.family: Theme.fontFamily
            font.pixelSize: 24
            font.weight: Font.Bold
            font.letterSpacing: 0
        }

        Rectangle {
            id: areaButton
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: 142
            height: 40
            radius: 6
            color: areaHoverHandler.hovered ? Theme.cyanDark : Theme.surfaceRaised
            border.width: 1
            border.color: areaHoverHandler.hovered ? Theme.borderStrong : Theme.border

            Behavior on color {
                ColorAnimation { duration: 120 }
            }

            Text {
                anchors.centerIn: parent
                text: root.areaText
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 16
                font.weight: Font.DemiBold
                font.letterSpacing: 0
            }

            HoverHandler {
                id: areaHoverHandler
                cursorShape: Qt.PointingHandCursor
            }

            TapHandler { id: areaTapHandler }
        }

        Row {
            anchors.right: settingsButton.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            height: parent.height
            spacing: 0

            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: 170
                text: root.dateTimeText
                color: Theme.text
                horizontalAlignment: Text.AlignHCenter
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.DemiBold
                font.letterSpacing: 0
            }

            Rectangle {
                width: 1
                height: parent.height
                color: Theme.border
            }

            StatusLabel {
                title: "통신 상태"
                status: root.systemStatusText
                statusColor: root.systemStatusColor
            }

            Rectangle {
                width: 1
                height: parent.height
                color: Theme.border
            }

            StatusLabel {
                title: "CCTV 상태"
                status: root.cctvStatusText
                statusColor: root.cctvStatusColor
            }
        }

        Rectangle {
            id: settingsButton
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 54
            height: 46
            radius: 6
            color: settingsHoverHandler.hovered ? Theme.cyanDark : Theme.surfaceRaised
            border.width: 1
            border.color: settingsHoverHandler.hovered ? Theme.borderStrong : Theme.border

            Behavior on color {
                ColorAnimation { duration: 120 }
            }

            Image {
                anchors.centerIn: parent
                width: 28
                height: 28
                source: "qrc:/icons/config_icon.png"
                sourceSize: Qt.size(56, 56)
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
            }

            HoverHandler {
                id: settingsHoverHandler
                cursorShape: Qt.PointingHandCursor
            }

            TapHandler { id: settingsTapHandler }
        }
    }
}
