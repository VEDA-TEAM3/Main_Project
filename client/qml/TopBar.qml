import QtQuick

TopBarForm {
    id: root

    /// 관제사 역할로 로그인하면 설정 버튼 자체를 감춘다. 누를 수 없는 버튼을 보여주는 것보다
    /// 아예 없는 편이 낫고, C++ 쪽 openMapSettingsDialog()에도 같은 검사가 한 번 더 있다
    property bool settingsAllowed: true

    signal areaRequested
    signal settingsRequested

    settingsButton.visible: root.settingsAllowed

    areaTapHandler.onTapped: root.areaRequested()
    settingsTapHandler.onTapped: root.settingsRequested()
}
