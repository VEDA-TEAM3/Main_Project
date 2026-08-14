pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import "Theme.js" as Theme

/** 설정창에서 쓰는 입력 상자입니다. 선택 상자와 같은 테두리·여백을 씁니다. */
TextField {
    id: root

    implicitHeight: 34
    leftPadding: 12
    rightPadding: 12
    selectByMouse: true
    color: Theme.text
    placeholderTextColor: Theme.textMuted
    font.family: Theme.fontFamily
    font.pixelSize: 14
    font.weight: Font.DemiBold
    font.letterSpacing: 0
    selectionColor: Theme.cyanDark
    selectedTextColor: Theme.text

    background: Rectangle {
        radius: 4
        color: Theme.background
        border.width: 1
        border.color: root.activeFocus ? Theme.borderStrong : Theme.border

        Behavior on border.color { ColorAnimation { duration: 110 } }
    }

    HoverHandler { cursorShape: Qt.IBeamCursor }
}
