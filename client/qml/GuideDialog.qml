pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic as Controls
import "Theme.js" as Theme

/**
 * 상단 표시줄 "?" 버튼과 F1로 여는 사용 안내 창입니다.
 * MapSettingsDialog와 같은 이유로 자식 위젯이 아니라 최상위 창으로 띄웁니다.
 * 내용은 이 파일 한 곳에만 있고, 관리자 전용 항목은 adminMode로 걸러냅니다.
 */
PanelFrame {
    id: root

    /// 관제사 계정에는 설정 버튼 자체가 없으므로 설정 안내도 감춥니다.
    property bool adminMode: true

    signal closed

    implicitWidth: 720
    implicitHeight: 620
    radius: 0
    color: Theme.surfaceRaised
    border.color: Theme.borderStrong

    /// 안내 본문입니다. adminOnly 항목은 관리자에게만 보입니다.
    readonly property var sections: [
        {
            "title": "1. 화면 구성",
            "adminOnly": false,
            "body": "<b>상단 표시줄</b> — 구역 전환, 현재 시각, 통신·CCTV 연결 상태<br>"
                    + "<b>CCTV 실시간 모니터링</b> — 지금 보고 있는 구역의 4개 채널 영상과 신고 버튼<br>"
                    + "<b>디지털 트윈 2D 맵</b> — 주차장 도면 위에 사람과 차량의 실시간 위치<br>"
                    + "<b>실시간 객체 목록 · 이벤트 로그</b> — 지금 잡혀 있는 객체와 지나간 사건<br>"
                    + "<b>장비 상태</b> — 채널별 LED와 알림 장치의 동작 상태"
        },
        {
            "title": "2. 상단 표시줄",
            "adminOnly": false,
            "body": "가운데 <b>제 N구역</b> 버튼을 누르면 구역 선택 창이 열립니다. 구역을 바꾸면 "
                    + "CCTV 4분할이 그 구역의 채널로 통째로 바뀝니다.<br>"
                    + "<b>통신 상태</b>는 관제 서버 연결입니다. 빨간색이면 지도·객체 목록·이벤트 로그가 갱신되지 않습니다.<br>"
                    + "<b>CCTV 상태</b>는 영상 연결입니다. 빨간색이면 영상 타일이 검은 채로 남습니다.<br>"
                    + "오른쪽 <b>톱니바퀴</b>는 설정이며 관리자 계정에서만 보입니다. "
                    + "관제사 계정에 버튼이 없는 것은 고장이 아닙니다.<br>"
                    + "맨 오른쪽 <b>?</b>가 이 안내이고, <b>F1</b>로도 열 수 있습니다."
        },
        {
            "title": "3. CCTV 모니터링",
            "adminOnly": false,
            "body": "<b>영상을 두 번 누르면 그 채널만 크게 확대</b>되고, 다시 두 번 누르면 4분할로 돌아옵니다. "
                    + "확대 중에는 나머지 채널의 영상 처리가 멈춥니다.<br>"
                    + "<b>CH 01 ~ CH 04</b> 버튼을 누르면 확인 창이 뜨고, 확인하면 안전 센터로 신고가 전송됩니다.<br>"
                    + "신고에는 <b>버튼을 누른 시점의 채널 위험 단계가 자동으로</b> 실립니다. 따로 고르지 않습니다.<br>"
                    + "전송하는 동안에는 네 버튼이 모두 잠기고, 실패하면 사유가 팝업으로 표시됩니다.<br>"
                    + "사람 얼굴과 차량 번호판은 자동으로 가려집니다. 설정에서 각각 끌 수 있습니다."
        },
        {
            "title": "4. 위험 색상",
            "adminOnly": false,
            "body": "<font color=\"" + Theme.safe + "\"><b>● 정상</b></font> 안전 상태 &nbsp; "
                    + "<font color=\"" + Theme.warning + "\"><b>● 주의</b></font> 유의 필요 &nbsp; "
                    + "<font color=\"" + Theme.danger + "\"><b>● 위험</b></font> 즉시 대응 필요<br>"
                    + "이 세 가지 색은 네 곳에서 모두 같은 뜻으로 쓰입니다 — "
                    + "지도 위 객체 표시, 이벤트 로그의 '위험 수준', CCTV 타일 테두리, 장비 상태의 LED 표시."
        },
        {
            "title": "5. 지도와 표 읽는 법",
            "adminOnly": false,
            "body": "<b>디지털 트윈 2D 맵</b>은 구역 격자 안에 객체의 실시간 위치를 그립니다. "
                    + "이동 경로·LED·CCTV·알림 장치 표시는 설정에서 켜고 끕니다.<br>"
                    + "<b>실시간 객체 목록</b> — ID · 유형 · 위치 (X, Y) · 구역 · 채널. 지금 잡혀 있는 객체입니다.<br>"
                    + "<b>이벤트 로그</b> — 시간 · 구역 · 채널 · 이벤트 · 위험 수준 · 조치 사항. 지나간 사건입니다.<br>"
                    + "<b>장비 상태</b> — 채널마다 SAFE / WARNING / DANGER와 LED·알림 장치의 ON / OFF를 보여 줍니다."
        },
        {
            "title": "6. 설정 (관리자 전용)",
            "adminOnly": true,
            "body": "<b>UI 설정</b> — 지도에 무엇을 그릴지, 아이콘 크기, 블러 대상(얼굴·번호판)<br>"
                    + "<b>영상 설정</b> — 채널별 밝기·대비·감마. 선택 채널, 현재 구역, 전체 채널 단위로 적용합니다.<br>"
                    + "<b>구역 관리</b> — 구역 추가와 영상 주소·계정 수정<br>"
                    + "<b>구역을 추가하거나 고치면 프로그램을 다시 시작해야 반영됩니다.</b> "
                    + "아직 반영되지 않은 구역은 목록에 '재시작 후 적용'으로 표시됩니다."
        },
        {
            "title": "7. 문제가 생기면",
            "adminOnly": false,
            "body": "<b>통신 상태가 빨간색</b> — 관제 서버에 연결되지 않은 상태입니다. "
                    + "지도와 목록이 멈춘 것처럼 보이면 여기부터 확인합니다.<br>"
                    + "<b>한 채널만 검게 남음</b> — 그 채널의 영상 주소나 계정 문제입니다. 설정의 구역 관리에서 확인합니다.<br>"
                    + "<b>지도의 객체가 한 점에 뭉침</b> — 좌표 경계가 잘못 잡힌 경우입니다. 설정 파일의 고정 경계를 확인해야 합니다.<br>"
                    + "<b>Alt + Enter</b>로 전체 화면을 켜고 끌 수 있습니다."
        }
    ]

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
                text: "사용 안내"
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 20
                font.weight: Font.Bold
                font.letterSpacing: 0
            }

            NeonButton {
                anchors.right: parent.right
                width: 42
                height: 38
                text: "×"
                onClicked: root.closed()
            }
        }

        Rectangle {
            id: headerLine

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.topMargin: 4
            height: 1
            color: Theme.border
        }

        Flickable {
            id: body

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: headerLine.bottom
            anchors.topMargin: 16
            anchors.bottom: footer.top
            anchors.bottomMargin: 16
            clip: true
            contentWidth: width
            contentHeight: content.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: content

                // 스크롤 막대가 본문 글자를 가리지 않도록 오른쪽을 비워 둡니다.
                width: body.width - 16
                spacing: 20

                Repeater {
                    model: root.sections

                    Column {
                        id: section

                        required property var modelData

                        width: content.width
                        spacing: 7
                        visible: !section.modelData.adminOnly || root.adminMode

                        PanelHeader {
                            width: section.width
                            titleText: section.modelData.title
                        }

                        Text {
                            width: section.width
                            text: section.modelData.body
                            color: Theme.textMuted
                            textFormat: Text.RichText
                            wrapMode: Text.WordWrap
                            lineHeight: 1.35
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.letterSpacing: 0
                        }
                    }
                }
            }

            Controls.ScrollBar.vertical: Controls.ScrollBar {
                policy: Controls.ScrollBar.AsNeeded

                contentItem: Rectangle {
                    implicitWidth: 6
                    radius: 3
                    color: Theme.border
                }
            }
        }

        Row {
            id: footer

            anchors.right: parent.right
            anchors.bottom: parent.bottom
            spacing: 10

            NeonButton {
                width: 112
                text: "닫기"
                selected: true
                onClicked: root.closed()
            }
        }
    }
}
