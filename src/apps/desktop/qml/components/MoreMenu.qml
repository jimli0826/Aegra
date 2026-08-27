pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15
import ".."

Popup {
    id: root

    width: 204
    height: root.menuItems.length * 44 + topPadding + bottomPadding
    padding: 8
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    signal helpFeedbackClicked()
    signal aboutClicked()
    signal checkUpdatesClicked()

    readonly property var menuItems: [
        { label: qsTrId("aegra.shell.help_feedback"), icon: "help_circle" },
        { label: qsTrId("aegra.shell.about"), icon: "info" },
        { label: qsTrId("aegra.shell.check_updates"), icon: "refresh" }
    ]

    background: Rectangle {
        radius: Theme.radiusControl
        color: Qt.rgba(Theme.colorPopup.r, Theme.colorPopup.g,
                       Theme.colorPopup.b, 1.0)
        border.width: 1
        border.color: Theme.colorBorder
    }

    contentItem: Column {
        id: menuColumn
        width: root.availableWidth

        Repeater {
            model: root.menuItems

            delegate: Rectangle {
                id: menuItem
                required property var modelData
                required property int index

                width: root.availableWidth
                height: 44
                radius: Theme.radiusControl
                color: itemMouse.containsMouse ? Theme.colorHover : "transparent"

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    spacing: 12

                    NavIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18
                        height: 18
                        name: menuItem.modelData.icon
                        color: itemMouse.containsMouse ? Theme.colorAccentBlue : Theme.colorTextGrey
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: menuItem.modelData.label
                        color: Theme.colorTextWhite
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                    }
                }

                MouseArea {
                    id: itemMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.close()
                        if (menuItem.index === 0) {
                            root.helpFeedbackClicked()
                        } else if (menuItem.index === 1) {
                            root.aboutClicked()
                        } else if (menuItem.index === 2) {
                            root.checkUpdatesClicked()
                        }
                    }
                }
            }
        }
    }
}
