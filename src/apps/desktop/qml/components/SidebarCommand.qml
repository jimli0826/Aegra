import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

Rectangle {
    id: root
    required property string label
    required property string icon
    required property bool collapsed

    Layout.fillWidth: true
    Layout.preferredHeight: 44
    radius: 2
    color: commandMouse.containsMouse && enabled ? Theme.colorHover : "transparent"

    Row {
        visible: !root.collapsed
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 12

        Text {
            text: root.icon
            color: Theme.colorAccentBlue
            font.family: "Segoe MDL2 Assets"
            font.pixelSize: 16
        }
        Text {
            text: root.label
            color: Theme.colorTextWhite
            font.family: Theme.fontFamily
            font.pixelSize: 14
        }
    }

    Text {
        visible: root.collapsed
        anchors.centerIn: parent
        text: root.icon
        color: Theme.colorAccentBlue
        font.family: "Segoe MDL2 Assets"
        font.pixelSize: 16
    }

    MouseArea {
        id: commandMouse
        anchors.fill: parent
        enabled: root.enabled
        hoverEnabled: true
        ToolTip.visible: root.collapsed && containsMouse
        ToolTip.text: root.label
    }
}
