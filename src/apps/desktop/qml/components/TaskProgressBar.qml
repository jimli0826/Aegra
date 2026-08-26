import QtQuick 2.15
import ".."

Item {
    id: root
    property real value: 0
    property bool active: false
    property bool indeterminate: false
    property color fillColor: active ? Theme.colorAccentBlue : Theme.colorTextDim
    width: parent ? parent.width : 120
    height: 8
    clip: true
    //% "Task progress %1 percent"
    Accessible.name: qsTrId("aegra.task.progress.accessible").arg(Math.round(value))
    Accessible.role: Accessible.ProgressBar

    Rectangle {
        anchors.fill: parent
        radius: 6
        color: Theme.colorProgressTrack
        border.width: 0
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: parent.width * Math.max(0, Math.min(value / 100.0, 1.0))
        radius: 6
        color: root.fillColor
        visible: !root.indeterminate && (root.active || value > 0)
    }

    Rectangle {
        id: slide
        visible: root.indeterminate
        width: Math.max(24, parent.width * 0.28)
        height: parent.height
        radius: 6
        color: root.fillColor
        SequentialAnimation on x {
            running: root.visible && root.indeterminate
            loops: Animation.Infinite
            NumberAnimation {
                from: -slide.width
                to: root.width
                duration: 1400
                easing.type: Easing.InOutSine
            }
        }
    }
}
