import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

PanelFrame {
    width: 800
    height: 640
    color: Theme.surfaceRaised
    border.color: Theme.borderStrong

    Column {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 14

        Text {
            text: "설정"
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 24
            font.weight: Font.Bold
        }

        TabBar {
            id: tabs
            width: parent.width

            TabButton { text: "UI 설정" }
            TabButton { text: "영상 설정" }
        }

        StackLayout {
            width: parent.width
            height: 480
            currentIndex: tabs.currentIndex

            PanelFrame {
                color: Theme.background

                Grid {
                    anchors.fill: parent
                    anchors.margins: 28
                    columns: 2
                    columnSpacing: 80
                    rowSpacing: 24

                    CheckBox { text: "이동 경로 표시"; checked: true }
                    CheckBox { text: "LED 표시"; checked: true }
                    CheckBox { text: "CCTV 표시"; checked: true }
                    CheckBox { text: "알림 장치 표시"; checked: true }
                    CheckBox { text: "CCTV 테두리 알림 표시"; checked: true }
                    CheckBox { text: "얼굴 블러"; checked: true }
                    CheckBox { text: "차량 번호판 블러"; checked: true }
                }
            }

            PanelFrame {
                color: Theme.background

                Column {
                    anchors.fill: parent
                    anchors.margins: 28
                    spacing: 20

                    CheckBox { text: "영상 전처리 사용"; checked: true }
                    ComboBox { width: parent.width; model: ["사용자 설정", "주간", "야간"] }
                    LabeledSlider { title: "밝기"; from: -20; to: 20; value: 0 }
                    LabeledSlider { title: "대비"; from: 80; to: 120; value: 100 }
                    LabeledSlider { title: "감마"; from: 80; to: 140; value: 100 }
                }
            }
        }
    }

    component LabeledSlider: Row {
        required property string title
        property alias from: slider.from
        property alias to: slider.to
        property alias value: slider.value
        width: parent.width
        spacing: 18

        Text { width: 80; text: parent.title; color: Theme.text; font.pixelSize: 14 }
        Slider { id: slider; width: parent.width - 170 }
        Text { width: 54; text: slider.value; color: Theme.text; horizontalAlignment: Text.AlignRight }
    }
}
