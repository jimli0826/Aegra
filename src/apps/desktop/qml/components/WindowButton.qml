import QtQuick 2.15
import ".."

// Matches backup/src/gui WindowButton: 36x32 hover chrome.
Rectangle {
    id: root
    property string icon: ""
    property bool isClose: false
    signal clicked()

    width: 36
    height: 32
    color: btnMouse.containsMouse
           ? (isClose ? Theme.colorHoverClose : Theme.colorHover)
           : "transparent"

    Text {
        anchors.centerIn: parent
        text: root.icon
        color: Theme.colorTextWhite
        font.pixelSize: 12
        font.family: Theme.fontFamily
    }

    MouseArea {
        id: btnMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
