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

    // 2px blurred soft-edge divider line with vertical fade gradient
    Item {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: 2

        // Soft outer feather blur with vertical gradient
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 0.20; color: Theme.colorSidebarDivider }
                GradientStop { position: 0.80; color: Theme.colorSidebarDivider }
                GradientStop { position: 1.0; color: "transparent" }
            }
            opacity: 0.12
        }

        // Inner core line with vertical gradient
        Rectangle {
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            width: 1
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 0.15; color: Theme.colorSidebarDivider }
                GradientStop { position: 0.85; color: Theme.colorSidebarDivider }
                GradientStop { position: 1.0; color: "transparent" }
            }
            opacity: 0.20
        }
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
        //% "Task Log"
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
                        GradientStop { position: 0.0; color: Theme.colorMenuActive }
                        GradientStop { position: 1.0; color: Theme.colorMenuActiveEnd }
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

            // 2px blurred soft-edge horizontal divider with fade gradient
            Item {
                anchors.centerIn: parent
                width: parent.width - (root.collapsed ? 4 : 16)
                height: 2

                // Soft outer feather blur with horizontal gradient
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: "transparent" }
                        GradientStop { position: 0.20; color: Theme.colorSidebarDivider }
                        GradientStop { position: 0.80; color: Theme.colorSidebarDivider }
                        GradientStop { position: 1.0; color: "transparent" }
                    }
                    opacity: 0.12
                }

                // Inner core line with horizontal gradient
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    height: 1
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: "transparent" }
                        GradientStop { position: 0.15; color: Theme.colorSidebarDivider }
                        GradientStop { position: 0.85; color: Theme.colorSidebarDivider }
                        GradientStop { position: 1.0; color: "transparent" }
                    }
                    opacity: 0.20
                }
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
    }
}
