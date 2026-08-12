pragma ComponentBehavior: Bound

import QtQuick
import "Theme.js" as Theme

/**
 * 설정 팝업입니다. 값은 전부 컨트롤이 직접 들고 있고 C++은 alias로 읽고 씁니다.
 * 사용자가 만진 경우에만 신호를 올려 C++이 프로그램적 갱신과 구분할 수 있게 합니다.
 */
PanelFrame {
    id: root

    property alias showMovementTrails: movementTrailsCheck.checked
    property alias showLed: ledCheck.checked
    property alias showCctv: cctvCheck.checked
    property alias showAlertDevice: alertDeviceCheck.checked
    property alias videoRiskBorders: riskBorderCheck.checked
    property alias faceBlur: faceBlurCheck.checked
    property alias licensePlateBlur: plateBlurCheck.checked

    property alias preprocessingEnabled: enabledCheck.checked
    property alias areaNames: areaCombo.model
    property alias areaIndex: areaCombo.currentIndex
    property alias channelNames: channelCombo.model
    property alias channelIndex: channelCombo.currentIndex
    property alias presetIndex: presetCombo.currentIndex
    property alias brightness: brightnessSlider.value
    property alias contrast: contrastSlider.value
    property alias gamma: gammaSlider.value

    signal applied
    signal cancelled
    signal areaSelected(int index)
    signal channelSelected(int index)
    signal presetSelected(int index)
    signal adjusted
    signal preprocessingToggled(bool enabled)
    signal resetRequested
    signal applySelectedRequested
    signal applyAreaRequested
    signal applyAllRequested

    implicitWidth: 800
    implicitHeight: 640
    radius: 0
    color: Theme.surfaceRaised
    border.color: Theme.borderStrong

    Item {
        anchors.fill: parent
        anchors.leftMargin: 32
        anchors.rightMargin: 32
        anchors.topMargin: 22
        anchors.bottomMargin: 22

        Item {
            id: header

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 40

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "설정"
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
                onClicked: root.cancelled()
            }
        }

        Row {
            id: tabBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.topMargin: 6
            height: 40
            spacing: 0

            property int currentIndex: 0

            Repeater {
                model: ["UI 설정", "영상 설정"]

                Rectangle {
                    id: tab

                    required property int index
                    required property string modelData
                    width: tabBar.width / 2
                    height: tabBar.height
                    color: tabBar.currentIndex === tab.index ? Theme.surface : Theme.background

                    Text {
                        anchors.centerIn: parent
                        text: tab.modelData
                        color: tabBar.currentIndex === tab.index ? Theme.text : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 2
                        color: tabBar.currentIndex === tab.index ? Theme.cyan : Theme.border
                    }

                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: tabBar.currentIndex = tab.index }
                }
            }
        }

        Row {
            id: footer

            anchors.right: parent.right
            anchors.bottom: parent.bottom
            spacing: 10

            NeonButton {
                width: 104
                text: "취소"
                onClicked: root.cancelled()
            }

            NeonButton {
                width: 112
                text: "적용"
                selected: true
                onClicked: root.applied()
            }
        }

        Rectangle {
            id: body

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: tabBar.bottom
            anchors.bottom: footer.top
            anchors.bottomMargin: 12
            color: Theme.surface
            border.width: 1
            border.color: Theme.border

            // ── UI 설정 탭 ─────────────────────────────────────────────
            Item {
                id: uiTab

                anchors.fill: parent
                anchors.margins: 22
                visible: tabBar.currentIndex === 0

                /** 제목 줄 하나를 얹은 구역 상자입니다. 높이는 내용에 맞춰 잡힙니다. */
                component SettingsGroup: Rectangle {
                    id: group

                    default property alias content: contentColumn.data
                    required property string title

                    implicitHeight: 16 + groupTitle.height + 14 + contentColumn.implicitHeight + 18
                    color: Theme.background
                    radius: 4
                    border.width: 1
                    border.color: Theme.border

                    Text {
                        id: groupTitle

                        anchors.left: parent.left
                        anchors.leftMargin: 20
                        anchors.top: parent.top
                        anchors.topMargin: 16
                        text: group.title
                        color: Theme.cyan
                        font.family: Theme.fontFamily
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        font.letterSpacing: 0
                    }

                    Column {
                        id: contentColumn

                        anchors.left: parent.left
                        anchors.leftMargin: 20
                        anchors.right: parent.right
                        anchors.rightMargin: 20
                        anchors.top: groupTitle.bottom
                        anchors.topMargin: 14
                        spacing: 16
                    }
                }

                Column {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 16

                    SettingsGroup {
                        id: mapGroup

                        width: parent.width
                        title: "맵 표시 설정"

                        Grid {
                            width: parent.width
                            columns: 2
                            columnSpacing: 40
                            rowSpacing: 20

                            OptionCheckBox { id: movementTrailsCheck; width: (mapGroup.width - 80) / 2; text: "이동 경로 표시" }
                            OptionCheckBox { id: ledCheck; width: (mapGroup.width - 80) / 2; text: "LED 표시" }
                            OptionCheckBox { id: cctvCheck; width: (mapGroup.width - 80) / 2; text: "CCTV 표시" }
                            OptionCheckBox { id: alertDeviceCheck; width: (mapGroup.width - 80) / 2; text: "알림 장치 표시" }
                        }
                    }

                    Item {
                        width: parent.width
                        // 두 상자는 내용이 달라도 같은 높이로 맞춥니다.
                        height: Math.max(cctvGroup.implicitHeight, blurGroup.implicitHeight)

                        SettingsGroup {
                            id: cctvGroup

                            anchors.left: parent.left
                            anchors.right: parent.horizontalCenter
                            anchors.rightMargin: 8
                            height: parent.height
                            title: "CCTV 알림 설정"

                            OptionCheckBox { id: riskBorderCheck; text: "CCTV 테두리 알림 표시" }
                        }

                        SettingsGroup {
                            id: blurGroup

                            anchors.left: parent.horizontalCenter
                            anchors.leftMargin: 8
                            anchors.right: parent.right
                            height: parent.height
                            title: "블러 설정"

                            OptionCheckBox { id: faceBlurCheck; text: "얼굴" }
                            OptionCheckBox { id: plateBlurCheck; text: "차량 번호판" }
                        }
                    }
                }
            }

            // ── 영상 설정 탭 ───────────────────────────────────────────
            Item {
                id: videoTab

                anchors.fill: parent
                anchors.leftMargin: 28
                anchors.rightMargin: 28
                anchors.topMargin: 16
                anchors.bottomMargin: 18
                visible: tabBar.currentIndex === 1

                component FieldLabel: Text {
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0
                }

                component SectionLabel: Text {
                    color: Theme.cyan
                    font.family: Theme.fontFamily
                    font.pixelSize: 15
                    font.weight: Font.Bold
                    font.letterSpacing: 0
                }

                Column {
                    id: videoColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    spacing: 12

                    Item {
                        width: parent.width
                        height: 34

                        SectionLabel {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: "영상 전처리"
                        }

                        OptionCheckBox {
                            id: enabledCheck

                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: "사용"
                            onClicked: root.preprocessingToggled(enabledCheck.checked)
                        }
                    }

                    Item {
                        width: parent.width
                        height: 34

                        FieldLabel {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: "적용 구역"
                        }

                        OptionComboBox {
                            id: areaCombo

                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            onActivated: root.areaSelected(areaCombo.currentIndex)
                        }
                    }

                    Item {
                        width: parent.width
                        height: 34

                        FieldLabel {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: "적용 대상"
                        }

                        OptionComboBox {
                            id: channelCombo

                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            onActivated: root.channelSelected(channelCombo.currentIndex)
                        }
                    }

                    Column {
                        id: adjustments

                        width: parent.width
                        spacing: 12
                        enabled: enabledCheck.checked
                        opacity: enabled ? 1.0 : 0.45

                        Behavior on opacity { NumberAnimation { duration: 120 } }

                        Item {
                            width: parent.width
                            height: 34

                            FieldLabel {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: "모드"
                            }

                            OptionComboBox {
                                id: presetCombo

                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                model: ["사용자 설정", "주간", "야간"]
                                onActivated: root.presetSelected(presetCombo.currentIndex)
                            }
                        }

                        component AdjustRow: Item {
                            id: adjustRow

                            required property string title
                            required property string valueText

                            width: adjustments.width
                            height: 30

                            FieldLabel {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: 60
                                text: adjustRow.title
                            }

                            Text {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width: 64
                                text: adjustRow.valueText
                                color: Theme.text
                                horizontalAlignment: Text.AlignHCenter
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0
                            }
                        }

                        AdjustRow {
                            title: "밝기"
                            valueText: (brightnessSlider.value > 0 ? "+" : "") + brightnessSlider.value

                            OptionSlider {
                                id: brightnessSlider

                                anchors.left: parent.left
                                anchors.leftMargin: 78
                                anchors.right: parent.right
                                anchors.rightMargin: 82
                                anchors.verticalCenter: parent.verticalCenter
                                from: -20
                                to: 20
                                onMoved: root.adjusted()
                            }
                        }

                        AdjustRow {
                            title: "대비"
                            valueText: (contrastSlider.value / 100).toFixed(2)

                            OptionSlider {
                                id: contrastSlider

                                anchors.left: parent.left
                                anchors.leftMargin: 78
                                anchors.right: parent.right
                                anchors.rightMargin: 82
                                anchors.verticalCenter: parent.verticalCenter
                                from: 80
                                to: 120
                                onMoved: root.adjusted()
                            }
                        }

                        AdjustRow {
                            title: "감마"
                            valueText: (gammaSlider.value / 100).toFixed(2)

                            OptionSlider {
                                id: gammaSlider

                                anchors.left: parent.left
                                anchors.leftMargin: 78
                                anchors.right: parent.right
                                anchors.rightMargin: 82
                                anchors.verticalCenter: parent.verticalCenter
                                from: 80
                                to: 140
                                onMoved: root.adjusted()
                            }
                        }
                    }
                }

                NeonButton {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    width: 124
                    text: "기본값 복원"
                    onClicked: root.resetRequested()
                }

                Row {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    spacing: 8

                    NeonButton {
                        width: 132
                        text: "선택 채널 적용"
                        enabled: enabledCheck.checked
                        onClicked: root.applySelectedRequested()
                    }

                    NeonButton {
                        width: 132
                        text: "현재 구역 적용"
                        enabled: enabledCheck.checked
                        onClicked: root.applyAreaRequested()
                    }

                    NeonButton {
                        width: 132
                        text: "전체 채널 적용"
                        selected: true
                        enabled: enabledCheck.checked
                        onClicked: root.applyAllRequested()
                    }
                }
            }
        }
    }
}
