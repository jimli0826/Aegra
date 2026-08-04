import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

// Visual baseline: backup/src/gui SidebarMenu.
Rectangle {
    id: root
    property int currentIndex: 0
    property bool settingsActive: false
    property bool collapsed: false
    property bool homeEnabled: true
    property bool backupEnabled: true
    property bool restoreEnabled: true
    property bool mountEnabled: true
    property bool repositoryEnabled: true
    property bool eventLogEnabled: true
    property bool settingsEnabled: true
    readonly property int expandedWidth: 160
    readonly property int collapsedWidth: 56
    readonly property int sideWidth: collapsed ? collapsedWidth : expandedWidth

    signal menuClicked(int index)
    signal settingsClicked()
    signal feedbackClicked()
    signal collapseToggled()

    color: Theme.colorSidebar
    clip: true

    readonly property var menuItems: [
        //% "Home"
        { label: qsTrId("aegra.nav.home"), icon: "\uE80F", index: 0, enabled: root.homeEnabled },
        //% "Backup"
        { label: qsTrId("aegra.nav.backup"), icon: "\uEA35", index: 1, enabled: root.backupEnabled },
        //% "Restore"
        { label: qsTrId("aegra.nav.restore"), icon: "\uE965", index: 2, enabled: root.restoreEnabled },
        //% "Mount"
        { label: qsTrId("aegra.nav.mount"), icon: "\uE8B9", index: 3, enabled: root.mountEnabled },
        //% "Repository"
        { label: qsTrId("aegra.nav.repository"), icon: "\uE8B7", index: 4, enabled: root.repositoryEnabled },
        //% "Event Log"
        { label: qsTrId("aegra.nav.event_log"), icon: "\uE7C3", index: 5, enabled: root.eventLogEnabled }
    ]

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 12
        anchors.bottomMargin: 8
        anchors.leftMargin: root.collapsed ? 6 : 12
        anchors.rightMargin: root.collapsed ? 6 : 12
        spacing: 0

        Repeater {
            model: root.menuItems
            delegate: Rectangle {
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                radius: 2
                color: !root.settingsActive && root.currentIndex === modelData.index
                       ? Theme.colorMenuActive
                       : (menuMouse.containsMouse && modelData.enabled
                          ? Theme.colorHover : "transparent")
                opacity: modelData.enabled ? 1.0 : 0.45

                Row {
                    visible: !root.collapsed
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12
                    Text {
                        text: modelData.icon
                        font.pixelSize: 18
                        font.family: "Segoe MDL2 Assets"
                        color: Theme.colorTextWhite
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: modelData.label
                        font.pixelSize: 14
                        font.family: Theme.fontFamily
                        color: Theme.colorTextWhite
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                Text {
                    visible: root.collapsed
                    anchors.centerIn: parent
                    text: modelData.icon
                    font.pixelSize: 18
                    font.family: "Segoe MDL2 Assets"
                    color: Theme.colorTextWhite
                }
                MouseArea {
                    id: menuMouse
                    anchors.fill: parent
                    enabled: modelData.enabled
                    hoverEnabled: true
                    cursorShape: modelData.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: root.menuClicked(modelData.index)
                    ToolTip.delay: 400
                    ToolTip.visible: root.collapsed && containsMouse
                    ToolTip.text: modelData.label
                }
            }
        }

        Item { Layout.fillHeight: true }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            radius: 2
            color: root.settingsActive ? Theme.colorMenuActive
                   : (settingsMouse.containsMouse ? Theme.colorHover : "transparent")
            opacity: root.settingsEnabled ? 1.0 : 0.55

            Row {
                visible: !root.collapsed
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12
                Text {
                    text: "\uE713"
                    font.pixelSize: 16
                    font.family: "Segoe MDL2 Assets"
                    color: Theme.colorAccentBlue
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    //% "Settings"
                    text: qsTrId("aegra.nav.settings")
                    font.pixelSize: 14
                    font.family: Theme.fontFamily
                    color: Theme.colorTextWhite
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Text {
                visible: root.collapsed
                anchors.centerIn: parent
                text: "\uE713"
                font.pixelSize: 16
                font.family: "Segoe MDL2 Assets"
                color: Theme.colorAccentBlue
            }
            MouseArea {
                id: settingsMouse
                anchors.fill: parent
                enabled: root.settingsEnabled
                hoverEnabled: true
                cursorShape: root.settingsEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: root.settingsClicked()
                ToolTip.delay: 400
                ToolTip.visible: root.collapsed && containsMouse
                //% "Settings"
                ToolTip.text: qsTrId("aegra.nav.settings")
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            radius: 2
            color: feedbackMouse.containsMouse ? Theme.colorHover : "transparent"
            opacity: 0.55

            Row {
                visible: !root.collapsed
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12
                Text {
                    text: "\uE715"
                    font.pixelSize: 16
                    font.family: "Segoe MDL2 Assets"
                    color: Theme.colorAccentBlue
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    //% "Feedback"
                    text: qsTrId("aegra.nav.feedback")
                    font.pixelSize: 14
                    font.family: Theme.fontFamily
                    color: Theme.colorTextWhite
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Text {
                visible: root.collapsed
                anchors.centerIn: parent
                text: "\uE715"
                font.pixelSize: 16
                font.family: "Segoe MDL2 Assets"
                color: Theme.colorAccentBlue
            }
            MouseArea {
                id: feedbackMouse
                anchors.fill: parent
                enabled: false
                hoverEnabled: true
                ToolTip.delay: 400
                ToolTip.visible: root.collapsed && containsMouse
                //% "Feedback"
                ToolTip.text: qsTrId("aegra.nav.feedback")
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            Layout.topMargin: 4
            radius: 2
            color: collapseMouse.containsMouse ? Theme.colorHover : "transparent"
            Text {
                anchors.centerIn: parent
                text: root.collapsed ? "\uE76C" : "\uE76B"
                font.pixelSize: 14
                font.family: "Segoe MDL2 Assets"
                color: Theme.colorTextWhite
            }
            MouseArea {
                id: collapseMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    root.collapsed = !root.collapsed
                    root.collapseToggled()
                }
                ToolTip.delay: 400
                ToolTip.visible: containsMouse
                //% "Expand menu"
                //% "Collapse menu"
                ToolTip.text: root.collapsed
                              ? qsTrId("aegra.nav.expand_sidebar")
                              : qsTrId("aegra.nav.collapse_sidebar")
            }
        }
    }
}
