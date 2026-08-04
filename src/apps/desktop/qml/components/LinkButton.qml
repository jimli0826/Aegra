import QtQuick 2.15
import ".."

// Text link matching backup/src/gui LinkButton.
Text {
    id: root
    signal clicked()

    color: linkMouse.containsMouse ? "#33b8ff" : Theme.colorAccentBlue
    font.pixelSize: 12
    font.family: Theme.fontFamily
    font.bold: true

    MouseArea {
        id: linkMouse
        anchors.fill: parent
        anchors.margins: -4
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
