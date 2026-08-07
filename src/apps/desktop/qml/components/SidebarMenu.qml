import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

// Glass sidebar — nav hover/active matches docs/design/index.html .nav-item
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
    readonly property int expandedWidth: 208
    readonly property int collapsedWidth: 72
    readonly property int sideWidth: collapsed ? collapsedWidth : expandedWidth

    signal menuClicked(int index)
    signal settingsClicked()
    signal feedbackClicked()
    signal collapseToggled()

    color: "transparent"
    clip: true

    Rectangle {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: 1
        color: Theme.colorBorder
        opacity: 0.55
    }

    readonly property var menuItems: [
        //% "Home"
        { label: qsTrId("aegra.nav.home"), icon: "home", index: 0, enabled: root.homeEnabled },
        //% "Backup"
        { label: qsTrId("aegra.nav.backup"), icon: "backup", index: 1, enabled: root.backupEnabled },
        //% "Restore"
        { label: qsTrId("aegra.nav.restore"), icon: "restore", index: 2, enabled: root.restoreEnabled },
        //% "Mount"
        { label: qsTrId("aegra.nav.mount"), icon: "mount", index: 3, enabled: root.mountEnabled },
        //% "Repository"
        { label: qsTrId("aegra.nav.repository"), icon: "repository", index: 4, enabled: root.repositoryEnabled },
        //% "Event Log"
        { label: qsTrId("aegra.nav.event_log"), icon: "event_log", index: 5, enabled: root.eventLogEnabled }
    ]

    readonly property bool serviceOk: typeof serviceClient !== "undefined"
                                      && serviceClient && serviceClient.connected
    readonly property string serviceTitle: root.serviceOk
        //% "Service is running"
        ? qsTrId("aegra.shell.service_running")
        //% "Service offline"
        : qsTrId("aegra.shell.service_offline")
    readonly property string serviceSub: root.serviceOk
        //% "Last sync · just now"
        ? qsTrId("aegra.shell.last_sync_just_now")
        : (typeof serviceClient !== "undefined" && serviceClient
           ? serviceClient.statusText : "")

    function itemActive(index) {
        return !root.settingsActive && root.currentIndex === index
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 28
        anchors.bottomMargin: 16
        anchors.leftMargin: root.collapsed ? 10 : 14
        anchors.rightMargin: root.collapsed ? 10 : 14
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: root.collapsed ? 40 : 44
            Layout.bottomMargin: 28
            Layout.leftMargin: root.collapsed ? 0 : 4

            Row {
                anchors.verticalCenter: parent.verticalCenter
                anchors.horizontalCenter: root.collapsed ? parent.horizontalCenter : undefined
                anchors.left: root.collapsed ? undefined : parent.left
                spacing: 11

                Rectangle {
                    width: 32
                    height: 32
                    radius: 10
                    anchors.verticalCenter: parent.verticalCenter
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#1a6b72" }
                        GradientStop { position: 1.0; color: "#2a9aa3" }
                    }
                    Rectangle {
                        x: 8; y: 7; width: 16; height: 5; radius: 2
                        color: "transparent"; border.width: 2; border.color: "#ffffff"
                        rotation: 180
                    }
                    Rectangle {
                        x: 8; y: 20; width: 16; height: 5; radius: 2
                        color: "transparent"; border.width: 2; border.color: "#ffffff"
                    }
                }

                Text {
                    visible: !root.collapsed
                    anchors.verticalCenter: parent.verticalCenter
                    //% "Aegra"
                    text: qsTrId("aegra.app.title")
                    color: Theme.colorTextWhite
                    font.pixelSize: 20
                    font.bold: true
                    font.family: Theme.fontFamily
                }
            }
        }

        Repeater {
            model: root.menuItems
            delegate: SidebarNavItem {
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                Layout.bottomMargin: 6
                active: root.itemActive(modelData.index)
                itemEnabled: modelData.enabled
                iconName: modelData.icon
                label: modelData.label
                showLabel: !root.collapsed
                onActivated: root.menuClicked(modelData.index)

                ToolTip.delay: 400
                ToolTip.visible: root.collapsed && hovered
                ToolTip.text: modelData.label
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            Rectangle {
                anchors.centerIn: parent
                width: parent.width - (root.collapsed ? 4 : 16)
                height: 1
                color: Theme.colorBorder
                opacity: 0.7
            }
        }

        Item { Layout.fillHeight: true }

        SidebarNavItem {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            Layout.bottomMargin: 6
            active: root.settingsActive
            itemEnabled: root.settingsEnabled
            iconName: "settings"
            //% "Settings"
            label: qsTrId("aegra.nav.settings")
            showLabel: !root.collapsed
            onActivated: root.settingsClicked()
            ToolTip.delay: 400
            ToolTip.visible: root.collapsed && hovered
            //% "Settings"
            ToolTip.text: qsTrId("aegra.nav.settings")
        }

        SidebarNavItem {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            Layout.bottomMargin: 10
            active: false
            itemEnabled: false
            iconName: "feedback"
            //% "Feedback"
            label: qsTrId("aegra.nav.feedback")
            showLabel: !root.collapsed
            ToolTip.delay: 400
            ToolTip.visible: root.collapsed && hovered
            //% "Feedback"
            ToolTip.text: qsTrId("aegra.nav.feedback")
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.collapsed ? 40 : 58
            Layout.topMargin: 2
            radius: 14
            color: root.serviceOk ? "#dcefea" : "#f5e6e4"
            border.width: 1
            border.color: root.serviceOk ? "#c5e0d8" : "#efc8c4"

            Column {
                visible: !root.collapsed
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 14
                anchors.rightMargin: 12
                spacing: 4

                Row {
                    spacing: 8
                    Rectangle {
                        width: 7
                        height: 7
                        radius: 4
                        anchors.verticalCenter: parent.verticalCenter
                        color: root.serviceOk ? Theme.colorGreen : Theme.colorAccentRed
                    }
                    Text {
                        text: root.serviceTitle
                        color: root.serviceOk ? "#276b61" : "#a04038"
                        font.pixelSize: 12
                        font.bold: true
                        font.family: Theme.fontFamily
                        elide: Text.ElideRight
                        width: Math.max(40, parent.parent.width - 24)
                    }
                }
                Text {
                    text: root.serviceSub
                    color: root.serviceOk ? "#5a8078" : "#a06058"
                    font.pixelSize: 11
                    font.family: Theme.fontFamily
                    leftPadding: 15
                    elide: Text.ElideRight
                    width: parent.width
                }
            }

            Rectangle {
                visible: root.collapsed
                anchors.centerIn: parent
                width: 10
                height: 10
                radius: 5
                color: root.serviceOk ? Theme.colorGreen : Theme.colorAccentRed
            }

            MouseArea {
                anchors.fill: parent
                enabled: !root.serviceOk
                hoverEnabled: true
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: {
                    if (typeof serviceClient !== "undefined" && serviceClient
                            && serviceClient.reconnect)
                        serviceClient.reconnect()
                }
                ToolTip.delay: 400
                ToolTip.visible: root.collapsed && containsMouse
                ToolTip.text: root.serviceTitle
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            Layout.topMargin: 8
            radius: Theme.radiusMenu
            color: collapseMouse.containsMouse ? Theme.colorHover : "transparent"
            Behavior on color {
                ColorAnimation { duration: 200; easing.type: Easing.OutCubic }
            }
            Text {
                anchors.centerIn: parent
                text: root.collapsed ? "\uE76C" : "\uE76B"
                font.pixelSize: 13
                font.family: "Segoe MDL2 Assets"
                color: Theme.colorTextDim
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
