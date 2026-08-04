import QtQuick 2.15
import ".."

Item {
    id: root
    property real value: 0
    property bool active: false
    width: parent ? parent.width : 120
    height: 8
    //% "Task progress %1 percent"
    Accessible.name: qsTrId("aegra.task.progress.accessible").arg(Math.round(value))
    Accessible.role: Accessible.ProgressBar

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: Theme.colorButton
        border.width: 1
        border.color: Theme.colorBorder
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: parent.width * Math.max(0, Math.min(value / 100.0, 1.0))
        radius: 4
        color: root.active ? Theme.colorAccentBlue : Theme.colorTextDim
        visible: root.active || value > 0
    }
}
