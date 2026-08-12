import QtQuick

TopBarForm {
    id: root

    signal areaRequested
    signal settingsRequested

    areaTapHandler.onTapped: root.areaRequested()
    settingsTapHandler.onTapped: root.settingsRequested()
}
