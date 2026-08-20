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
    property string channelText: ""
    property string riskText: ""
    property var choices: []
    property int selectedIndex: 0
    /** 창을 여는 순간에는 전환을 꺼서 현재 구역이 곧바로 선택된 상태로 보이게 합니다. */
    property bool selectionAnimated: true
    signal accepted(int selectedIndex)
    signal rejected

    readonly property bool reportMode: root.mode === "confirmation" || root.mode === "success"
    readonly property color riskColor: root.riskText === "위험" ? Theme.danger
                                                                : (root.riskText === "주의" ? Theme.warning : Theme.safe)

    // 구역 선택은 3열 2행 버튼 여섯 개가 전부다. 버튼 하나가 148px이 되도록 거꾸로 잡은
    // 크기다 (칸 164 x 3열 + 좌우 여백 68)
    implicitWidth: root.mode === "area" ? 560 : 470
    implicitHeight: root.mode === "area" ? 280 : (root.reportMode ? 288 : 230)
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
                    mipmap: true
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

                // 신고 확인/완료는 대상 채널과 현재 위험 상태를 함께 보여 줍니다.
                Rectangle {
                    anchors.fill: parent
                    visible: root.reportMode
                    color: Theme.background
                    border.width: 1
                    border.color: Theme.border
                    radius: 4

                    Column {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 14

                        Row {
                            width: parent.width
                            height: 34
                            spacing: 10

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: channelLabel.implicitWidth + 22
                                height: 30
                                radius: 3
                                color: Theme.surfaceRaised
                                border.width: 1
                                border.color: Theme.cyan

                                Text {
                                    id: channelLabel

                                    anchors.centerIn: parent
                                    text: root.channelText
                                    color: Theme.cyan
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 15
                                    font.weight: Font.Bold
                                    font.letterSpacing: 0
                                }
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "현재 상태"
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0
                            }

                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 9
                                    height: 9
                                    radius: 4.5
                                    color: root.riskColor
                                }

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: root.riskText
                                    color: root.riskColor
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 14
                                    font.weight: Font.Bold
                                    font.letterSpacing: 0
                                }
                            }
                        }

                        Rectangle {
                            width: parent.width
                            height: 1
                            color: Theme.border
                            opacity: 0.7
                        }

                        Text {
                            width: parent.width
                            text: root.messageText
                            color: Theme.text
                            wrapMode: Text.WordWrap
                            lineHeight: 1.25
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0
                        }
                    }
                }

                // 구역은 3열 2행에 순서대로 배치하고, 미사용 칸은 비워 둡니다.
                // 칸 수는 도면 격자(ParkingPlan.js의 ZONE_COLS x ZONE_ROWS)와 같아야 합니다.
                Rectangle {
                    anchors.fill: parent
                    visible: root.mode === "area"
                    color: Theme.background
                    border.width: 1
                    border.color: Theme.border
                    radius: 4

                    GridView {
                        id: choiceGrid

                        anchors.fill: parent
                        anchors.margins: 12
                        cellWidth: width / 3
                        cellHeight: height / 2
                        model: 6
                        currentIndex: root.selectedIndex
                        clip: true

                        delegate: Item {
                            id: choiceDelegate

                            required property int index
                            readonly property bool available: index < root.choices.length
                            width: choiceGrid.cellWidth
                            height: choiceGrid.cellHeight

                            NeonButton {
                                anchors.centerIn: parent
                                // 칸 크기를 따라가므로 창 크기를 바꾸면 버튼도 같이 조절된다.
                                // 높이는 아래 취소·선택 버튼과 같은 38에 맞춰 한 벌로 보이게 한다
                                width: parent.width - 16
                                height: Math.min(38, parent.height - 12)
                                visible: choiceDelegate.available
                                text: choiceDelegate.available ? root.choices[choiceDelegate.index] : ""
                                selected: choiceDelegate.available && root.selectedIndex === choiceDelegate.index
                                animated: root.selectionAnimated
                                onClicked: root.selectedIndex = choiceDelegate.index
                            }
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    width: parent.width
                    visible: !root.reportMode && root.mode !== "area" && root.messageText.length > 0
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
            }
    }
}
