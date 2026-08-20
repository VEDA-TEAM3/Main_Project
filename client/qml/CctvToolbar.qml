pragma ComponentBehavior: Bound

import QtQuick
import "Theme.js" as Theme

Item {
    id: root

    property string titleText: "CCTV Live Monitoring"
    property bool reportEnabled: true
    signal reportRequested(int channelNumber)

    implicitHeight: 36
    clip: true

    PanelHeader {
        anchors.left: parent.left
        anchors.right: actionRow.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        titleText: root.titleText
    }

    Row {
        id: actionRow

        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 7

        Rectangle {
            // 폭은 내용에 맞춰 늘립니다. 숫자로 박아 두면 글꼴이 조금만 넓어져도 "Report"가
            // 잘려 나가는데, 눈에는 글자가 아니라 여백이 없는 것으로 보입니다
            width: reportContent.width + 24
            height: 34
            radius: 5
            color: Theme.surfaceRaised
            border.width: 1
            border.color: Theme.borderStrong

            Row {
                id: reportContent

                anchors.centerIn: parent
                spacing: 6

                Image {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 20
                    height: 20
                    source: "qrc:/icons/report_icon.png"
                    sourceSize: Qt.size(40, 40)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    mipmap: true
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Report"
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
