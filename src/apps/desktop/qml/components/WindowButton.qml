import QtQuick 2.15
import ".."

Rectangle {
    id: root
    property string icon: ""
    property bool isClose: false
    signal clicked()

    width: 36
    height: 32
    color: buttonMouse.containsMouse
           ? (root.isClose ? Theme.colorHoverClose : Theme.colorHover)
           : "transparent"

    Text {
        anchors.centerIn: parent
        text: root.icon
        color: Theme.colorTextWhite
        font.family: "Segoe MDL2 Assets"
        font.pixelSize: 11
    }

    MouseArea {
        id: buttonMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
