pragma ComponentBehavior: Bound

import QtQuick
import "Theme.js" as Theme

/**
 * 실시간 객체 목록과 이벤트 로그가 공유하는 표입니다.
 * C++ QAbstractTableModel을 그대로 붙이고, 열 너비는 columnWidthProvider로 정합니다.
 */
Item {
    id: root

    property var tableModel: null
    /** 열 개수와 비율은 여기서 정하고, 열 제목은 model의 headerData()에서 그대로 읽습니다. */
    property var columnWeights: []
    property int headerHeight: 28
    property int rowHeight: 26

    Rectangle {
        id: header

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.headerHeight
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.tableHeaderTop }
            GradientStop { position: 0.22; color: Theme.tableHeaderMid }
            GradientStop { position: 1.0; color: Theme.tableHeaderBottom }
        }

        Row {
            anchors.fill: parent

            Repeater {
                model: root.columnWeights.length

                Item {
                    id: headerCell

                    required property int index
                    width: header.width * root.columnWeights[headerCell.index]
                    height: header.height

                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 8
                        text: root.tableModel ? root.tableModel.headerData(headerCell.index, Qt.Horizontal) : ""
                        color: Theme.tableHeaderText
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Font.Bold
                    }
                }
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.tableHeaderLine
        }
    }

    TableView {
        id: view

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        model: root.tableModel
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        rowHeightProvider: function (row) { return root.rowHeight }
        columnWidthProvider: function (column) { return view.width * root.columnWeights[column] }

        onWidthChanged: view.forceLayout()

        delegate: Item {
            id: cell

            required property int row
            required property int column
            // 행이 새로 생기는 순간 역할 값이 채워지기 전 한 프레임 undefined입니다.
            // color/string으로 선언하면 그 프레임에 "undefined" 글자와 QML 경고가 쏟아지므로
            // var로 받고 아래에서 기본값으로 막습니다.
            required property var display
            required property var textColor
            required property var rowColor
            required property var iconSource

            readonly property string cellText: cell.display === undefined ? "" : cell.display
            readonly property string cellIconSource: cell.iconSource === undefined ? "" : cell.iconSource

            Rectangle {
                anchors.fill: parent
                // 모델이 위험 행 배경을 지정하지 않으면 홀수 행만 옅게 깔아 줄을 구분합니다.
                color: (cell.rowColor !== undefined && cell.rowColor.a > 0)
                       ? cell.rowColor
                       : (cell.row % 2 === 1 ? Theme.tableRowAlt : "transparent")

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: Theme.tableRowLine
                }
            }

            Row {
                anchors.centerIn: parent
                spacing: cellIcon.visible ? 6 : 0

                Image {
                    id: cellIcon

                    anchors.verticalCenter: parent.verticalCenter
                    width: 16
                    height: 16
                    visible: cell.cellIconSource.length > 0
                    source: cell.cellIconSource
                    sourceSize: Qt.size(32, 32)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, cell.width - 8 - (cellIcon.visible ? 22 : 0))
                    text: cell.cellText
                    color: cell.textColor === undefined ? Theme.text : cell.textColor
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    // TableView의 시각적 자식은 contentItem으로 들어가 함께 스크롤되므로 표 밖에 둡니다.
    Rectangle {
        anchors.right: view.right
        anchors.rightMargin: 1
        width: 4
        radius: 2
        color: Theme.cyanDark
        visible: view.contentHeight > view.height
        y: view.y + view.visibleArea.yPosition * view.height
        height: Math.max(24, view.visibleArea.heightRatio * view.height)
        opacity: 0.8
    }
}
