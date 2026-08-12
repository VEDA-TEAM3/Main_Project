import QtQuick
import "Theme.js" as Theme

/**
 * 대시보드 카드가 공유하는 제목 줄입니다. 좌측 강조 막대와 제목 글꼴을
 * 한 곳에서 정의해 CCTV/맵/객체 목록/이벤트 로그/장비 상태가 같은 모양을 갖습니다.
 */
Item {
    id: root

    property string titleText: ""
    property int titlePixelSize: 16

    implicitHeight: 28
    implicitWidth: accentBar.width + label.anchors.leftMargin + label.implicitWidth

    Rectangle {
        id: accentBar

        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: root.titlePixelSize + 5
        color: Theme.cyan
    }

    Text {
        id: label

        anchors.left: accentBar.right
        anchors.leftMargin: 8
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        text: root.titleText
        color: Theme.text
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: root.titlePixelSize
        font.weight: Font.Bold
        font.letterSpacing: 0
    }
}
