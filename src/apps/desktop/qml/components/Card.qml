import QtQuick 2.15
import QtQuick.Layouts 1.15
import ".."

Rectangle {
    id: root
    property string title: ""

    color: Theme.colorCard
    radius: 4
    border.width: 1
    border.color: Theme.colorBorder

    // Title with blue accent
    Row {
        x: 12
        y: 12
        spacing: 8

        Rectangle {
            width: 3
            height: 18
            color: Theme.colorAccentBlue
        }

        Text {
            text: root.title
            color: Theme.colorTextWhite
            font.pixelSize: 14
            font.bold: true
            font.family: Theme.fontFamily
            anchors.verticalCenter: parent.children[0].verticalCenter
        }
    }
}
