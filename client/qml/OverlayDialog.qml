pragma ComponentBehavior: Bound

import QtQuick
import "Theme.js" as Theme

// 뒤 화면을 가리지 않도록 딤 배경 없이 패널 크기의 창으로 띄웁니다.
// C++ 쪽에서 implicit 크기를 읽어 창 크기를 맞춥니다.
PanelFrame {
    id: root

    property string mode: "information"
    property string titleText: "안내"
    property string messageText: ""
    property var choices: []
    property int selectedIndex: 0
    signal accepted(int selectedIndex)
    signal rejected

    implicitWidth: root.mode === "area" ? 590 : 460
    implicitHeight: root.mode === "area" ? 280 : 230
    radius: 0
    color: Theme.surfaceRaised
    border.color: Theme.borderStrong

    Item {
        anchors.fill: parent
        anchors.margins: 22

            Item {
                id: header

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 40

                Image {
                    id: reportIcon
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 30
                    height: 30
                    visible: root.mode === "confirmation" || root.mode === "success"
                    source: visible ? "qrc:/icons/report_icon.png" : ""
                    sourceSize: Qt.size(60, 60)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }

                Text {
                    anchors.left: reportIcon.visible ? reportIcon.right : parent.left
                    anchors.leftMargin: reportIcon.visible ? 8 : 0
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.titleText
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 21
                    font.weight: Font.Bold
                    font.letterSpacing: 0
                }

                NeonButton {
                    anchors.right: parent.right
                    width: 42
                    height: 38
                    text: "×"
                    onClicked: root.rejected()
                }
            }

            Row {
                id: footer

                anchors.right: parent.right
                anchors.bottom: parent.bottom
                spacing: 10

                NeonButton {
                    visible: root.mode !== "success" && root.mode !== "information"
                    width: 104
                    text: "취소"
                    onClicked: root.rejected()
                }

                NeonButton {
                    width: 112
                    text: root.mode === "area" ? "선택" : "확인"
                    selected: true
                    onClicked: root.accepted(root.selectedIndex)
                }
            }

            Item {
                id: body

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: header.bottom
                anchors.bottom: footer.top
                anchors.topMargin: 10
                anchors.bottomMargin: 12

                Column {
                    anchors.centerIn: parent
                    width: parent.width
                    spacing: root.mode === "area" ? 10 : 0

                    Text {
                        width: parent.width
                        visible: root.messageText.length > 0
                        text: root.messageText
                        color: Theme.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0
                    }

                    GridView {
                        id: choiceGrid

                        visible: root.mode === "area"
                        width: parent.width
                        height: visible ? 104 : 0
                        cellWidth: width / 4
                        cellHeight: 48
                        model: root.choices
                        currentIndex: root.selectedIndex
                        clip: true

                        delegate: Item {
                            id: choiceDelegate

                            required property int index
                            required property var modelData
                            width: choiceGrid.cellWidth
                            height: choiceGrid.cellHeight

                            NeonButton {
                                anchors.centerIn: parent
                                width: parent.width - 8
                                height: 38
                                text: choiceDelegate.modelData
                                selected: root.selectedIndex === choiceDelegate.index
                                onClicked: root.selectedIndex = choiceDelegate.index
                            }
                        }
                    }
                }
            }
    }
}
