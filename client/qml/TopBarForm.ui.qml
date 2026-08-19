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
    property alias helpTapHandler: helpTapHandler
    property alias settingsButton: settingsButton

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
            anchors.right: helpButton.left
            // 관제사 계정에서는 감춰지므로, 그 자리가 빈칸으로 남지 않도록 폭까지 접습니다
            anchors.rightMargin: settingsButton.visible ? 6 : 0
            anchors.verticalCenter: parent.verticalCenter
            width: settingsButton.visible ? 54 : 0
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

        // 사용 안내 버튼입니다. 설정과 달리 역할을 가리지 않으므로, 관제사 계정에서도
        // 오른쪽 끝이 비지 않도록 설정 버튼보다 바깥쪽에 둡니다.
        Rectangle {
            id: helpButton
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 40
            height: 40
            radius: width / 2
            color: helpHoverHandler.hovered ? Theme.cyanDark : Theme.surfaceRaised
            border.width: 1
            border.color: helpHoverHandler.hovered ? Theme.borderStrong : Theme.border
            scale: helpTapHandler.pressed ? 0.94 : 1.0

            Behavior on color {
                ColorAnimation { duration: 120 }
            }

            Behavior on border.color {
                ColorAnimation { duration: 120 }
            }

            Behavior on scale {
                NumberAnimation { duration: 70; easing.type: Easing.OutCubic }
            }

            // 커서를 올렸을 때 바깥으로 번지는 테두리. 부모를 clip하지 않아 버튼 밖에 그려집니다.
            Rectangle {
                anchors.centerIn: parent
                width: parent.width + 10
                height: parent.height + 10
                radius: width / 2
                color: "transparent"
                border.width: 1
                border.color: Theme.borderStrong
                opacity: helpHoverHandler.hovered ? 0.5 : 0.0

                Behavior on opacity {
                    NumberAnimation { duration: 150 }
                }
            }

            Text {
                anchors.centerIn: parent
                text: "?"
                color: helpHoverHandler.hovered ? Theme.text : Theme.cyan
                font.family: Theme.fontFamily
                font.pixelSize: 22
                font.weight: Font.Bold
                font.letterSpacing: 0
            }

            HoverHandler {
                id: helpHoverHandler
                cursorShape: Qt.PointingHandCursor
            }

            TapHandler { id: helpTapHandler }
        }
    }
}
