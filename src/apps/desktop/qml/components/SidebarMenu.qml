import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

Rectangle {
    id: root
    property int currentIndex: 4
    property bool collapsed: false
    readonly property int expandedWidth: 160
    readonly property int collapsedWidth: 56
    readonly property int sideWidth: collapsed ? collapsedWidth : expandedWidth
    signal menuClicked(int index)

    color: Theme.colorSidebar
    clip: true

    readonly property var menuItems: [
        { label: "首页", icon: "\uE80F", index: 0, enabled: false },
        { label: "备份", icon: "\uEA35", index: 1, enabled: false },
        { label: "恢复", icon: "\uE965", index: 2, enabled: false },
        { label: "挂载", icon: "\uE8B9", index: 3, enabled: false },
        { label: "Repository", icon: "\uE8B7", index: 4, enabled: true },
        { label: "事件日志", icon: "\uE7C3", index: 5, enabled: false }
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
                color: root.currentIndex === modelData.index
                       ? Theme.colorMenuActive
                       : (menuMouse.containsMouse && modelData.enabled
                          ? Theme.colorHover : "transparent")

                Row {
                    visible: !root.collapsed
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12

                    Text {
                        text: modelData.icon
                        color: Theme.colorTextWhite
                        font.family: "Segoe MDL2 Assets"
                        font.pixelSize: 18
                    }

                    Text {
                        text: modelData.label
                        color: Theme.colorTextWhite
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                    }
                }

                Text {
                    visible: root.collapsed
                    anchors.centerIn: parent
                    text: modelData.icon
                    color: Theme.colorTextWhite
                    font.family: "Segoe MDL2 Assets"
                    font.pixelSize: 18
                }

                MouseArea {
                    id: menuMouse
                    anchors.fill: parent
                    enabled: modelData.enabled
                    hoverEnabled: true
                    cursorShape: modelData.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: root.menuClicked(modelData.index)
                    ToolTip.visible: root.collapsed && containsMouse
                    ToolTip.text: modelData.label
                }
            }
        }

        Item { Layout.fillHeight: true }

        SidebarCommand {
            label: "设置"
            icon: "\uE713"
            collapsed: root.collapsed
            enabled: false
        }

        SidebarCommand {
            label: "反馈"
            icon: "\uE715"
            collapsed: root.collapsed
            enabled: false
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
                color: Theme.colorTextWhite
                font.family: "Segoe MDL2 Assets"
                font.pixelSize: 14
            }

            MouseArea {
                id: collapseMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.collapsed = !root.collapsed
                ToolTip.visible: containsMouse
                ToolTip.text: root.collapsed ? "展开侧栏" : "折叠侧栏"
            }
        }
    }
}
