pragma ComponentBehavior: Bound

import QtQuick
import "Theme.js" as Theme

/**
 * 프로그램 시작 시 대시보드를 덮는 로그인 화면입니다.
 *
 * 대시보드는 페이드할 수 없습니다 - 영상 타일이 네이티브 창이라 QGraphicsEffect가 걸리지
 * 않습니다. 그래서 전환 연출은 이 화면이 걷히는 쪽에서만 만들고, 실제 창 투명도는
 * exitFinished를 받은 C++이 처리합니다.
 */
Rectangle {
    id: root

    /// 값은 컨트롤이 직접 들고 C++이 alias로 읽는다 (바인딩하면 사용자 입력에 끊긴다)
    property alias userName: userField.text
    property alias password: passwordField.text
    property alias passwordConfirm: confirmField.text
    /// 인증 실패 사유. 비어 있으면 자리만 차지하고 보이지 않는다
    property string errorText: ""
    /// 인증 처리 중에는 입력을 잠근다
    property bool busy: false
    /// 계정이 하나도 없는 설치본이면 로그인 대신 최초 관리자 생성으로 연다.
    /// 기본 계정을 심어 배포하면 현장에서 끝내 안 바뀌므로 여기서 직접 만들게 한다
    property bool bootstrapMode: false

    signal loginRequested
    signal bootstrapRequested
    signal exitStarted
    signal exitFinished

    implicitWidth: 1280
    implicitHeight: 800
    color: Theme.background

    /** @brief 인증 성공 후 화면이 걷히는 연출을 시작합니다. */
    function playExit() {
        if (exitAnimation.running) {
            return;
        }
        root.exitStarted();
        exitAnimation.start();
    }

    /** @brief 화면이 열릴 때 첫 입력 칸으로 초점을 옮깁니다. */
    function focusFirstField() {
        if (userField.text.length > 0) {
            passwordField.forceActiveFocus();
        } else {
            userField.forceActiveFocus();
        }
    }

    /** @brief 입력이 갖춰졌을 때만 인증 또는 계정 생성을 요청합니다. */
    function submit() {
        if (root.busy || userField.text.trim().length === 0 || passwordField.text.length === 0) {
            return;
        }

        if (!root.bootstrapMode) {
            root.loginRequested();
            return;
        }

        if (passwordField.text !== confirmField.text) {
            root.errorText = "Passwords do not match.";
            return;
        }
        root.bootstrapRequested();
    }

    // ── 배경 ──────────────────────────────────────────────────────────────
    // 셰이더(QtQuick.Effects)를 쓰지 않는다. 배포에 플러그인이 하나 더 붙고,
    // 여기서 얻는 것은 원형 그라디언트 하나뿐이다
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#0b1015" }
            GradientStop { position: 0.55; color: Theme.background }
            GradientStop { position: 1.0; color: "#101a21" }
        }
    }

    // 옅은 격자. 관제 화면의 결을 배경에도 얹는다
    Item {
        anchors.fill: parent
        opacity: 0.3

        Repeater {
            model: Math.ceil(root.width / 64)
            Rectangle {
                required property int index
                x: index * 64
                width: 1
                height: root.height
                color: "#1d2830"
            }
        }
        Repeater {
            model: Math.ceil(root.height / 64)
            Rectangle {
                required property int index
                y: index * 64
                width: root.width
                height: 1
                color: "#1d2830"
            }
        }
    }

    // 카드 뒤 발광. 원반을 쓰면 가장자리가 그대로 보이므로, 창 폭을 가로지르는 띠로
    // 세로 방향만 부드럽게 흐린다. 아주 느리게 숨쉬어 화면이 살아 있다는 느낌만 준다
    Rectangle {
        id: glow

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: 760
        opacity: 0.0
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#001b2f3a" }
            GradientStop { position: 0.5; color: "#ff1b2f3a" }
            GradientStop { position: 1.0; color: "#001b2f3a" }
        }

        SequentialAnimation on opacity {
            running: !exitAnimation.running
            loops: Animation.Infinite
            NumberAnimation { to: 0.55; duration: 3200; easing.type: Easing.InOutSine }
            NumberAnimation { to: 0.32; duration: 3200; easing.type: Easing.InOutSine }
        }
    }

    // 가장자리를 눌러 시선을 가운데로 모은다
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#66000000" }
            GradientStop { position: 0.45; color: "#00000000" }
            GradientStop { position: 1.0; color: "#73000000" }
        }
    }

    // ── 로그인 카드 ───────────────────────────────────────────────────────
    Rectangle {
        id: card

        anchors.centerIn: parent
        width: 440
        height: content.implicitHeight + 64
        radius: 10
        color: Theme.surface
        border.width: 1
        border.color: Theme.border

        // 등장/퇴장에서 함께 움직이는 값들.
        // y를 직접 애니메이션하면 centerIn 앵커와 충돌해 카드가 아래로 밀린다.
        // 앵커는 그대로 두고 오프셋만 움직인다
        opacity: 0.0
        scale: 0.97
        anchors.verticalCenterOffset: 20

        Column {
            id: content

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 40
            anchors.rightMargin: 40
            spacing: 0

            // 원본 300px을 54px로 줄여 그립니다. sourceSize가 없으면 300px 텍스처를 그대로
            // 올려 놓고 GPU가 5.5배로 줄이느라 가장자리가 깨집니다. 디코드 단계에서 2배
            // 크기로 미리 줄이고(고해상도 화면 몫), 나머지는 mipmap이 매끄럽게 받습니다
            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 54
                height: 54
                source: "qrc:/icons/main.png"
                sourceSize: Qt.size(108, 108)
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
            }

            Item { width: 1; height: 18 }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "PARKING VMS"
                color: Theme.cyan
                font.family: Theme.fontFamily
                font.pixelSize: 30
                font.weight: Font.Bold
                font.letterSpacing: 2
            }

            Item { width: 1; height: 6 }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Video Management System"
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 14
                font.weight: Font.Medium
            }

            Item { width: 1; height: 26 }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.border
                opacity: 0.6
            }

            Item { width: 1; height: 26 }

            // 최초 실행 안내. Column은 visible이 false인 자식을 배치에서 아예 건너뛴다
            Text {
                visible: root.bootstrapMode
                width: parent.width
                text: "No operator account exists on this machine yet.\nCreate an administrator account to continue."
                color: Theme.cyan
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.DemiBold
                lineHeight: 1.35
                wrapMode: Text.WordWrap
            }

            Item { visible: root.bootstrapMode; width: 1; height: 22 }

            Text {
                text: "Username"
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }

            Item { width: 1; height: 8 }

            OptionTextField {
                id: userField
                width: parent.width
                height: 42
                enabled: !root.busy
                placeholderText: "Operator account"
                onAccepted: passwordField.forceActiveFocus()
                onTextChanged: root.errorText = ""
            }

            Item { width: 1; height: 16 }

            Text {
                text: "Password"
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }

            Item { width: 1; height: 8 }

            OptionTextField {
                id: passwordField
                width: parent.width
                height: 42
                enabled: !root.busy
                echoMode: TextInput.Password
                placeholderText: "Password"
                onAccepted: root.bootstrapMode ? confirmField.forceActiveFocus() : root.submit()
                onTextChanged: root.errorText = ""
            }

            Item { visible: root.bootstrapMode; width: 1; height: 16 }

            Text {
                visible: root.bootstrapMode
                text: "Confirm password"
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }

            Item { visible: root.bootstrapMode; width: 1; height: 8 }

            OptionTextField {
                id: confirmField
                visible: root.bootstrapMode
                width: parent.width
                height: 42
                enabled: !root.busy
                echoMode: TextInput.Password
                placeholderText: "Re-enter password"
                onAccepted: root.submit()
                onTextChanged: root.errorText = ""
            }

            // 오류 문구 자리는 항상 잡아 둔다. 나타날 때 카드가 튀면 싸구려로 보인다
            Item {
                width: parent.width
                height: 34

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    text: root.errorText
                    color: Theme.danger
                    opacity: root.errorText.length > 0 ? 1.0 : 0.0
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap

                    Behavior on opacity { NumberAnimation { duration: 140 } }
                }
            }

            NeonButton {
                id: loginButton
                width: parent.width
                implicitHeight: 44
                enabled: !root.busy && userField.text.trim().length > 0 && passwordField.text.length > 0 &&
                         (!root.bootstrapMode || confirmField.text.length > 0)
                selected: enabled
                text: root.busy ? "Signing in..." : (root.bootstrapMode ? "Create administrator" : "Sign in")
                onClicked: root.submit()
            }

            Item { width: 1; height: 18 }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: root.bootstrapMode ? "Accounts are stored in config/users.json next to the program"
                                         : "Sign in to start receiving video and device status"
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 11
                opacity: 0.75
            }
        }
    }

    // ── 등장 ──────────────────────────────────────────────────────────────
    // 로고 -> 카드 순으로 살짝 어긋나게 올려 급하지 않은 인상을 만든다
    ParallelAnimation {
        id: entranceAnimation
        running: true

        NumberAnimation {
            target: card
            property: "opacity"
            to: 1.0
            duration: 420
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            target: card
            property: "scale"
            to: 1.0
            duration: 520
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            target: card
            property: "anchors.verticalCenterOffset"
            to: 0
            duration: 520
            easing.type: Easing.OutCubic
        }

        onFinished: root.focusFirstField()
    }

    // ── 퇴장 ──────────────────────────────────────────────────────────────
    // 카드가 떠오르며 사라지고 배경이 어두워진 뒤, 창 투명도는 C++이 마저 내린다
    SequentialAnimation {
        id: exitAnimation

        ParallelAnimation {
            NumberAnimation {
                target: card
                property: "opacity"
                to: 0.0
                duration: 260
                easing.type: Easing.InCubic
            }
            NumberAnimation {
                target: card
                property: "scale"
                to: 1.05
                duration: 320
                easing.type: Easing.InCubic
            }
            NumberAnimation {
                target: card
                property: "anchors.verticalCenterOffset"
                to: -30
                duration: 320
                easing.type: Easing.InCubic
            }
            NumberAnimation {
                target: glow
                property: "opacity"
                to: 0.0
                duration: 260
            }
        }

        NumberAnimation {
            target: blackout
            property: "opacity"
            to: 1.0
            duration: 220
            easing.type: Easing.InQuad
        }

        onFinished: root.exitFinished()
    }

    // 창을 걷어내는 순간은 결국 한 프레임에 끊긴다. 밝은 화면에서 끊으면 그 끊김이 그대로
    // 보이므로, 감추기 직전에 검은색으로 덮어 어두운 프레임에서 대시보드로 넘어가게 한다.
    // (창 자체를 페이드하거나 밀어내면 QQuickWidget 최상위 창이 죽는다 - LoginWindow 주석 참고)
    Rectangle {
        id: blackout
        anchors.fill: parent
        color: "#000000"
        opacity: 0.0
    }

    // 창 전체를 덮으므로 Esc로 닫히면 안 된다. Enter만 받는다
    Keys.onReturnPressed: root.submit()
    Keys.onEnterPressed: root.submit()
}
