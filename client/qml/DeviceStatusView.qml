pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import "Theme.js" as Theme

/**
 * 4채널 장비 상태 패널입니다.
 * C++(DeviceStatusPanel::refreshChannels)이 채널마다 아래 순서의 평평한 배열을 내려 줍니다.
 * [0] title [1] healthState [2] tooltip [3] ledSafe [4] ledWarning [5] ledDanger
 * [6] beaconOff [7] beaconOn [8] buzzerOff [9] buzzerOn
 * (QVariantMap을 쓰면 heap이 깨집니다 — CLAUDE.md 참고)
 */
Item {
    id: root

    property var channels: []

    Grid {
        anchors.fill: parent
        anchors.margins: 6
        columns: 2
        columnSpacing: 8
        rowSpacing: 6

        Repeater {
            model: root.channels

            Rectangle {
                id: card

                required property var modelData

                readonly property string title: card.modelData[0]
                readonly property string healthState: card.modelData[1]
                readonly property string tooltip: card.modelData[2]

                width: (root.width - 12 - 8) / 2
                height: (root.height - 12 - 6) / 2
                radius: 5
                border.width: 1
                border.color: card.healthState === "failed"
                              ? Theme.danger
                              : (card.healthState === "confirmed" ? Theme.deviceCardConfirmed
                                                                  : Theme.deviceCardBorder)

                gradient: Gradient {
                    GradientStop { position: 0.0; color: Theme.deviceCardTop }
                    GradientStop { position: 1.0; color: Theme.deviceCardBottom }
                }

                Behavior on border.color { ColorAnimation { duration: 140 } }

                ToolTip.visible: cardHover.hovered && card.tooltip.length > 0
                ToolTip.text: card.tooltip
                ToolTip.delay: 400

                HoverHandler { id: cardHover }

                Column {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    anchors.topMargin: 6
                    anchors.bottomMargin: 8
                    spacing: 5

                    Text {
                        text: card.title
                        color: Theme.cyan
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.Bold
                        font.letterSpacing: 0
                    }

                    StatusRow {
                        width: parent.width
                        height: (parent.height - root.titleHeight - parent.spacing * 3) / 3
                        iconSource: "qrc:/icons/led_icon.png"
                        tooltip: "LED 전광판"
                        segments: [
                            { text: "SAFE", kind: "safe", active: card.modelData[3] },
                            { text: "WARNING", kind: "warning", active: card.modelData[4] },
                            { text: "DANGER", kind: "danger", active: card.modelData[5] }
                        ]
                    }

                    StatusRow {
                        width: parent.width
                        height: (parent.height - root.titleHeight - parent.spacing * 3) / 3
                        iconSource: "qrc:/icons/siren_icon.png"
                        tooltip: "경광등"
                        segments: [
                            { text: "OFF", kind: "off", active: card.modelData[6] },
                            { text: "ON", kind: "on", active: card.modelData[7] }
                        ]
                    }

                    StatusRow {
                        width: parent.width
                        height: (parent.height - root.titleHeight - parent.spacing * 3) / 3
                        iconSource: "qrc:/icons/buzzer_icon.png"
                        tooltip: "부저"
                        segments: [
                            { text: "OFF", kind: "off", active: card.modelData[8] },
                            { text: "ON", kind: "on", active: card.modelData[9] }
                        ]
                    }
                }
            }
        }
    }

    /** 제목 줄 높이입니다. 세 상태 행이 카드의 남은 높이를 똑같이 나눠 갖게 하는 데 씁니다. */
    readonly property int titleHeight: 20

    component StatusRow: Rectangle {
        id: statusRow

        required property url iconSource
        required property string tooltip
        required property var segments

        color: Theme.deviceRowBackground
        radius: 4
        border.width: 1
        border.color: Theme.deviceRowBorder

        ToolTip.visible: rowHover.hovered
        ToolTip.text: statusRow.tooltip
        ToolTip.delay: 400

        HoverHandler { id: rowHover }

        Image {
            id: rowIcon

            anchors.left: parent.left
            anchors.leftMargin: 5
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            source: statusRow.iconSource
            sourceSize: Qt.size(40, 40)
            fillMode: Image.PreserveAspectFit
            smooth: true
        }

        Rectangle {
            id: rowSeparator

            anchors.left: rowIcon.right
            anchors.leftMargin: 5
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: 3
            anchors.bottomMargin: 3
            width: 1
            color: Theme.deviceSeparator
        }

        Row {
            id: segmentRow

            anchors.left: rowSeparator.right
            anchors.leftMargin: 5
            anchors.right: parent.right
            anchors.rightMargin: 5
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: 3
            anchors.bottomMargin: 3
            spacing: 5

            Repeater {
                model: statusRow.segments

                Rectangle {
                    id: segment

                    required property var modelData
                    readonly property bool isSafe: segment.modelData.kind === "safe" || segment.modelData.kind === "off"
                    readonly property bool isWarning: segment.modelData.kind === "warning"

                    width: (segmentRow.width - (statusRow.segments.length - 1) * segmentRow.spacing)
                           / statusRow.segments.length
                    height: segmentRow.height
                    radius: 3
                    color: !segment.modelData.active
                           ? Theme.deviceSegmentBackground
                           : (segment.isSafe ? Theme.deviceSafeBackground
                                             : (segment.isWarning ? Theme.deviceWarningBackground
                                                                  : Theme.deviceDangerBackground))
                    border.width: 1
                    border.color: !segment.modelData.active
                                  ? Theme.deviceSegmentBorder
                                  : (segment.isSafe ? Theme.safe
                                                    : (segment.isWarning ? Theme.warning : Theme.danger))

                    Behavior on color { ColorAnimation { duration: 160 } }
                    Behavior on border.color { ColorAnimation { duration: 160 } }

                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 6
                        text: segment.modelData.text
                        color: segment.modelData.active
                               ? (segment.isSafe ? Theme.deviceSafeText
                                                 : (segment.isWarning ? Theme.deviceWarningText
                                                                      : Theme.deviceDangerText))
                               : (segment.isSafe ? Theme.deviceSafeIdleText
                                                 : (segment.isWarning ? Theme.deviceWarningIdleText
                                                                      : Theme.deviceDangerIdleText))
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Font.Bold
                        font.letterSpacing: 0

                        Behavior on color { ColorAnimation { duration: 160 } }
                    }
                }
            }
        }
    }
}
