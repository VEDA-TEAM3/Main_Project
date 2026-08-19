pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as Controls
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
    property alias iconScalePercent: iconScaleSetting.value
    property alias movementTrailLength: trailLengthSetting.value
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

    /// 설정 파일에 저장된 구역 이름들. 뒤쪽 activeZoneCount개 밖은 아직 실행에 반영되지 않은 구역입니다.
    property var zoneNames: []
    /// 고른 구역을 이 화면에서 고칠 수 있는지. 채널마다 계정이 다르면 false입니다.
    property bool zoneEditable: true
    /// 값을 고쳐 저장했지만 아직 실행에 반영되지 않은 구역 번호들
    property var editedRows: []
    property int activeZoneCount: 0
    /// 지금 화면에 띄워 둔 구역. 목록에서 누르면 이 값이 그 구역으로 바뀝니다.
    property int currentZoneIndex: 0
    /// 목록에서 고른 줄. 실행 중인 구역이면 주소를 보여 주고, 빈 자리면 추가 입력을 보여 줍니다.
    property int selectedRow: 0
    property int zoneCapacity: 8
    property string zoneMessage: ""
    property bool zoneMessageError: false
    property alias zoneName: zoneNameField.text
    property alias zoneUser: zoneUserField.text
    property alias zonePassword: zonePasswordField.text
    property alias zoneUrl1: zoneUrlField1.text
    property alias zoneUrl2: zoneUrlField2.text
    property alias zoneUrl3: zoneUrlField3.text
    property alias zoneUrl4: zoneUrlField4.text

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
    signal zoneAddRequested
    signal zoneSaveRequested(int index)
    signal zoneSelectRequested(int index)

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
                model: ["UI 설정", "영상 설정", "구역 관리"]

                Rectangle {
                    id: tab

                    required property int index
                    required property string modelData
                    width: tabBar.width / 3
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
                onClicked: {
                    // 아직 확정되지 않은 숫자 입력을 먼저 값으로 옮긴 뒤 C++이 읽게 한다
                    iconScaleSetting.commit();
                    trailLengthSetting.commit();
                    root.applied();
                }
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

                /** 맵 표시 숫자 설정 한 줄입니다. */
                component MapNumberSetting: Item {
                    id: numberSetting

                    required property string title
                    property alias value: valueField.value

                    /**
                     * 편집 중인 글자를 값으로 확정합니다.
                     *
                     * SpinBox는 focus를 잃거나 엔터를 칠 때만 글자를 값으로 옮깁니다. 적용 버튼은
                     * TapHandler라 눌러도 focus가 옮겨가지 않아, 엔터를 치지 않으면 옛 값이 그대로
                     * 적용됐습니다. focus 이동 시점에 기대지 않도록 여기서 직접 확정합니다.
                     */
                    function commit() {
                        valueField.value = valueField.valueFromText(valueInput.text, valueField.locale);
                    }
                    property int minimumValue: 0
                    property int maximumValue: 999
                    property int stepSize: 1
                    property string unit: ""

                    implicitHeight: 44

                    Column {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2

                        Text {
                            text: numberSetting.title
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0
                        }

                        Text {
                            text: numberSetting.minimumValue + " ~ " + numberSetting.maximumValue + numberSetting.unit
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                            font.letterSpacing: 0
                        }
                    }

                    Controls.SpinBox {
                        id: valueField

                        anchors.right: parent.right
                        width: 104
                        height: 34
                        from: numberSetting.minimumValue
                        to: numberSetting.maximumValue
                        stepSize: numberSetting.stepSize
                        editable: true
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold

                        contentItem: TextInput {
                            id: valueInput

                            z: 2
                            leftPadding: 10
                            rightPadding: 31
                            text: valueField.textFromValue(valueField.value, valueField.locale)
                            color: Theme.text
                            selectionColor: Theme.cyanDark
                            selectedTextColor: Theme.text
                            horizontalAlignment: TextInput.AlignRight
                            verticalAlignment: TextInput.AlignVCenter
                            selectByMouse: true
                            readOnly: !valueField.editable
                            validator: valueField.validator
                            inputMethodHints: Qt.ImhDigitsOnly
                        }

                        up.indicator: Rectangle {
                            x: valueField.width - width
                            y: 1
                            width: 29
                            height: valueField.height / 2 - 1
                            color: valueField.up.pressed ? Theme.cyanDark :
                                   (valueField.up.hovered ? Theme.surfaceRaised : "transparent")

                            Text {
                                anchors.centerIn: parent
                                text: "▴"
                                color: valueField.up.hovered ? Theme.text : Theme.cyan
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                            }

                            Rectangle {
                                anchors.left: parent.left
                                width: 1
                                height: parent.height
                                color: Theme.border
                            }
                        }

                        down.indicator: Rectangle {
                            x: valueField.width - width
                            y: valueField.height / 2
                            width: 29
                            height: valueField.height / 2 - 1
                            color: valueField.down.pressed ? Theme.cyanDark :
                                   (valueField.down.hovered ? Theme.surfaceRaised : "transparent")

                            Text {
                                anchors.centerIn: parent
                                text: "▾"
                                color: valueField.down.hovered ? Theme.text : Theme.cyan
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                            }

                            Rectangle {
                                anchors.left: parent.left
                                width: 1
                                height: parent.height
                                color: Theme.border
                            }

                            Rectangle {
                                anchors.top: parent.top
                                width: parent.width
                                height: 1
                                color: Theme.border
                            }
                        }

                        background: Rectangle {
                            radius: 4
                            color: Theme.background
                            border.width: 1
                            border.color: valueField.activeFocus ? Theme.borderStrong : Theme.border
                        }
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
                            MapNumberSetting {
                                id: iconScaleSetting
                                width: (mapGroup.width - 80) / 2
                                title: "아이콘 크기"
                                value: 100
                                minimumValue: 50
                                maximumValue: 200
                                stepSize: 5
                                unit: "%"
                            }
                            MapNumberSetting {
                                id: trailLengthSetting
                                width: (mapGroup.width - 80) / 2
                                title: "이동 경로 길이"
                                value: 240
                                minimumValue: 40
                                maximumValue: 600
                                stepSize: 10
                            }
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

            // ── 구역 관리 탭 ───────────────────────────────────────────
            Item {
                id: zoneTab

                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 24
                anchors.topMargin: 16
                anchors.bottomMargin: 14
                visible: tabBar.currentIndex === 2

                /** 저장된 구역 수. 그중 앞의 activeZoneCount개만 현재 실행에 반영되어 있습니다. */
                readonly property int savedZoneCount: root.zoneNames.length
                readonly property bool canAddZone: zoneTab.savedZoneCount < root.zoneCapacity
                /// 오른쪽 폼이 고칠 구역. 빈 자리를 골랐으면 -1이라 새 구역 입력이 됩니다.
                readonly property int detailIndex: root.selectedRow < root.activeZoneCount ? root.selectedRow : -1
                readonly property bool editMode: zoneTab.detailIndex >= 0
                readonly property bool formEnabled: root.zoneEditable && (zoneTab.editMode || zoneTab.canAddZone)

                component ZonePanel: Rectangle {
                    id: zonePanel

                    default property alias content: panelColumn.data
                    required property string title

                    color: Theme.background
                    radius: 4
                    border.width: 1
                    border.color: Theme.border

                    Text {
                        id: panelTitle

                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        anchors.top: parent.top
                        anchors.topMargin: 14
                        text: zonePanel.title
                        color: Theme.cyan
                        font.family: Theme.fontFamily
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        font.letterSpacing: 0
                    }

                    Column {
                        id: panelColumn

                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        anchors.top: panelTitle.bottom
                        anchors.topMargin: 12
                        spacing: 8
                    }
                }

                /** 이름표 + 입력 칸 한 줄입니다. 구역 정보와 구역 추가가 같은 형식을 쓰도록 여기 둡니다. */
                component ZoneField: Item {
                    id: zoneField

                    default property alias input: fieldSlot.data
                    required property string label

                    width: parent.width
                    height: 34

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 62
                        text: zoneField.label
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0
                    }

                    Item {
                        id: fieldSlot

                        anchors.left: parent.left
                        anchors.leftMargin: 68
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        height: parent.height
                    }
                }

                /** 두 패널의 구역을 가르는 줄입니다. */
                component ZoneSeparator: Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.border
                    opacity: 0.6
                }

                // 사용 중인 구역 수를 칸으로 먼저 보여 준다. 남은 자리가 몇 개인지가 이 탭의 핵심 정보다
                Item {
                    id: capacityRow

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 26

                    Text {
                        id: capacityLabel

                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: "등록된 구역"
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0
                    }

                    Row {
                        anchors.left: capacityLabel.right
                        anchors.leftMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 5

                        Repeater {
                            model: root.zoneCapacity

                            Rectangle {
                                id: pip

                                required property int index
                                width: 22
                                height: 8
                                radius: 2
                                color: pip.index < root.activeZoneCount ? Theme.cyan
                                     : pip.index < zoneTab.savedZoneCount ? Theme.warning : Theme.surface
                                border.width: 1
                                border.color: pip.index < zoneTab.savedZoneCount ? "transparent" : Theme.border
                            }
                        }
                    }

                    Text {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: zoneTab.savedZoneCount + " / " + root.zoneCapacity
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.Bold
                        font.letterSpacing: 0
                    }
                }

                ZonePanel {
                    id: zoneListPanel

                    anchors.left: parent.left
                    anchors.top: capacityRow.bottom
                    anchors.topMargin: 12
                    anchors.bottom: zoneMessageText.top
                    anchors.bottomMargin: 10
                    width: 262
                    title: "구역 목록"

                    Repeater {
                        model: root.zoneCapacity

                        Item {
                            id: zoneRow

                            required property int index
                            readonly property bool active: zoneRow.index < root.activeZoneCount
                            readonly property bool pending: !zoneRow.active && zoneRow.index < zoneTab.savedZoneCount
                            /// 저장은 됐지만 실행에 반영되지 않은 상태(새로 추가했거나 값을 고친 구역)
                            readonly property bool restartNeeded: zoneRow.pending
                                                                  || root.editedRows.indexOf(zoneRow.index) >= 0
                            readonly property bool current: zoneRow.active && zoneRow.index === root.currentZoneIndex
                            // 채널 번호가 0부터 빈틈 없이 이어져야 하므로 빈 자리는 맨 앞의 하나만 채울 수 있습니다.
                            readonly property bool addTarget: !zoneRow.active && !zoneRow.pending
                                                              && zoneRow.index === zoneTab.savedZoneCount
                            readonly property bool selectable: zoneRow.active || zoneRow.addTarget
                            readonly property color tone: zoneRow.active ? Theme.cyan
                                                        : zoneRow.pending ? Theme.warning
                                                        : zoneRow.addTarget ? Theme.cyan : Theme.textMuted

                            width: parent.width
                            height: 30

                            Rectangle {
                                anchors.fill: parent
                                radius: 4
                                color: zoneRow.index === root.selectedRow && zoneRow.selectable ? Theme.cyanDark
                                     : zoneMouse.containsMouse ? Theme.surfaceRaised : "transparent"
                                border.width: 1
                                border.color: zoneRow.index === root.selectedRow && zoneRow.selectable
                                              ? Theme.borderStrong
                                            : zoneRow.addTarget ? Theme.border : "transparent"

                                Behavior on color { ColorAnimation { duration: 110 } }
                            }

                            // 실행 중인 구역을 누르면 그 구역 화면으로 가고, 빈 자리를 누르면 추가 입력으로 갑니다.
                            MouseArea {
                                id: zoneMouse

                                anchors.fill: parent
                                enabled: zoneRow.selectable
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.selectedRow = zoneRow.index
                                    // C++이 그 구역 값을 폼에 채우고, 실행 중인 구역이면 화면도 전환합니다
                                    root.zoneSelectRequested(zoneRow.index)
                                    Qt.callLater(zoneNameField.forceActiveFocus)
                                }
                            }

                            Rectangle {
                                id: zoneBadge

                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: 22
                                height: 22
                                radius: 11
                                color: zoneRow.current ? Theme.cyan : "transparent"
                                border.width: 1
                                border.color: zoneRow.tone
                                opacity: zoneRow.selectable || zoneRow.pending ? 1.0 : 0.35

                                Text {
                                    anchors.centerIn: parent
                                    text: zoneRow.index + 1
                                    color: zoneRow.current ? Theme.background : zoneRow.tone
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 11
                                    font.weight: Font.Bold
                                    font.letterSpacing: 0
                                }
                            }

                            Text {
                                anchors.left: zoneBadge.right
                                anchors.leftMargin: 10
                                anchors.right: zoneRowState.left
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: zoneRow.index < zoneTab.savedZoneCount ? root.zoneNames[zoneRow.index]
                                    : zoneRow.addTarget ? "여기에 구역 추가" : "확장 예정"
                                color: zoneRow.index < zoneTab.savedZoneCount ? Theme.text
                                     : zoneRow.addTarget ? Theme.cyan : Theme.textMuted
                                opacity: zoneRow.index < zoneTab.savedZoneCount || zoneRow.addTarget ? 1.0 : 0.45
                                elide: Text.ElideRight
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0
                            }

                            Text {
                                id: zoneRowState

                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: zoneRow.restartNeeded ? "재시작 후 적용" : zoneRow.current ? "보는 중" : ""
                                color: zoneRow.restartNeeded && !zoneRow.active ? Theme.warning : zoneRow.tone
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0
                            }
                        }
                    }
                }

                // 구역 추가와 수정이 같은 폼을 씁니다. 고른 줄이 실행 중인 구역이면 그 값이 채워집니다.
                ZonePanel {
                    id: zoneFormPanel

                    anchors.left: zoneListPanel.right
                    anchors.leftMargin: 14
                    anchors.right: parent.right
                    anchors.top: zoneListPanel.top
                    anchors.bottom: zoneListPanel.bottom
                    title: zoneTab.editMode
                           ? "구역 " + (zoneTab.detailIndex + 1) + " 수정"
                           : zoneTab.canAddZone ? "새 구역 추가 · 구역 " + (zoneTab.savedZoneCount + 1)
                                                : "새 구역 추가"

                    ZoneField {
                        label: "구역 이름"

                        OptionTextField {
                            id: zoneNameField

                            anchors.fill: parent
                            enabled: zoneTab.formEnabled
                            maximumLength: 40
                            placeholderText: zoneTab.editMode ? "" : "예: 제 " + (zoneTab.savedZoneCount + 1) + "구역"
                        }
                    }

                    // 계정은 주소와 분리해서 받습니다. 주소 칸에 넣으면 비밀번호가 관제 화면에
                    // 그대로 보이고, 특수문자가 섞인 비밀번호는 URL 파싱도 깨집니다.
                    Item {
                        width: parent.width
                        height: 34

                        Text {
                            id: userLabel

                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            width: 62
                            text: "계정"
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0
                        }

                        OptionTextField {
                            id: zoneUserField

                            anchors.left: userLabel.right
                            anchors.leftMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            width: (parent.width - 68 - 74 - 12) / 2
                            enabled: zoneTab.formEnabled
                            placeholderText: "admin"
                        }

                        Text {
                            id: passwordLabel

                            anchors.left: zoneUserField.right
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            width: 62
                            text: "비밀번호"
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0
                        }

                        OptionTextField {
                            id: zonePasswordField

                            anchors.left: passwordLabel.right
                            anchors.leftMargin: 6
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            enabled: zoneTab.formEnabled
                            echoMode: TextInput.Password
                        }
                    }

                    ZoneSeparator {}

                    ZoneField {
                        label: "CH 01"

                        OptionTextField {
                            id: zoneUrlField1

                            anchors.fill: parent
                            enabled: zoneTab.formEnabled
                            placeholderText: "rtsp://192.168.0.51:554/0/profile4/media.smp"
                            // 구역을 고르면 C++이 값을 넣는데, 그때 커서가 끝으로 가 앞부분이 밀려 나갑니다.
                            // 사용자가 입력 중일 때는(포커스가 있을 때) 건드리지 않습니다.
                            onTextChanged: if (!activeFocus) cursorPosition = 0
                        }
                    }

                    ZoneField {
                        label: "CH 02"

                        OptionTextField {
                            id: zoneUrlField2

                            anchors.fill: parent
                            enabled: zoneTab.formEnabled
                            onTextChanged: if (!activeFocus) cursorPosition = 0
                        }
                    }

                    ZoneField {
                        label: "CH 03"

                        OptionTextField {
                            id: zoneUrlField3

                            anchors.fill: parent
                            enabled: zoneTab.formEnabled
                            onTextChanged: if (!activeFocus) cursorPosition = 0
                        }
                    }

                    ZoneField {
                        label: "CH 04"

                        OptionTextField {
                            id: zoneUrlField4

                            anchors.fill: parent
                            enabled: zoneTab.formEnabled
                            onTextChanged: if (!activeFocus) cursorPosition = 0
                        }
                    }

                    Item {
                        width: parent.width
                        height: 40

                        Text {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: addZoneButton.left
                            anchors.rightMargin: 12
                            text: !root.zoneEditable ? "채널마다 계정이 달라 이 화면에서는 고칠 수 없습니다."
                                : zoneTab.editMode ? "저장하면 다시 시작할 때 적용됩니다. 계정은 네 채널에 함께 적용됩니다."
                                : zoneTab.canAddZone ? "구역은 앞자리부터 차례로 채워집니다. 계정은 네 채널에 함께 적용됩니다."
                                : "구역 자리를 모두 사용했습니다."
                            color: root.zoneEditable ? Theme.textMuted : Theme.warning
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            font.weight: Font.Normal
                            font.letterSpacing: 0
                        }

                        NeonButton {
                            id: addZoneButton

                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: 118
                            text: zoneTab.editMode ? "변경 저장" : "구역 추가"
                            selected: true
                            enabled: zoneTab.formEnabled && zoneNameField.text.trim().length > 0
                                     && zoneUrlField1.text.trim().length > 0 && zoneUrlField2.text.trim().length > 0
                                     && zoneUrlField3.text.trim().length > 0 && zoneUrlField4.text.trim().length > 0
                            onClicked: {
                                if (zoneTab.editMode) {
                                    root.zoneSaveRequested(zoneTab.detailIndex)
                                } else {
                                    root.zoneAddRequested()
                                }
                            }
                        }
                    }
                }

                Text {
                    id: zoneMessageText

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 20
                    text: root.zoneMessage
                    color: root.zoneMessageError ? Theme.danger : Theme.safe
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0
                }
            }
        }
    }
}
