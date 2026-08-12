pragma ComponentBehavior: Bound

import QtQuick
import "Theme.js" as Theme

Item {
    id: root

    property string titleText: "CCTV 실시간 모니터링"
    property bool reportEnabled: true
    signal reportRequested(int channelNumber)

    implicitHeight: 36
    clip: true

    // 다른 카드 제목과 같은 높이에 놓이도록 위쪽에 붙입니다.
    PanelHeader {
        anchors.left: parent.left
        anchors.right: actionRow.left
        anchors.rightMargin: 12
        anchors.top: parent.top
        titleText: root.titleText
    }

    Row {
        id: actionRow

        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 7

        Rectangle {
            width: 82
            height: 34
            radius: 5
            color: Theme.surfaceRaised
            border.width: 1
            border.color: Theme.borderStrong

            Row {
                anchors.centerIn: parent
                spacing: 5

                Image {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 20
                    height: 20
                    source: "qrc:/icons/report_icon.png"
                    sourceSize: Qt.size(40, 40)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "신고"
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0
                }
            }
        }

        Repeater {
            model: 4

            NeonButton {
                id: channelButton
                required property int index
                width: 74
                height: 34
                text: "CH " + String(index + 1).padStart(2, "0")
                enabled: root.reportEnabled
                onClicked: root.reportRequested(channelButton.index + 1)
            }
        }
    }
}
