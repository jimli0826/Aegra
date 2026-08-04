import QtQuick 2.15
import QtQuick.Layouts 1.15
import ".."

// Page title row matching backup/src/gui: 3px blue accent + 18px bold title.
RowLayout {
    id: root
    property string title: ""
    spacing: 12
    Layout.fillWidth: true

    Row {
        spacing: 8
        Layout.alignment: Qt.AlignVCenter

        Rectangle {
            width: 3
            height: 20
            color: Theme.colorAccentBlue
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            text: root.title
            color: Theme.colorTextWhite
            font.pixelSize: 18
            font.bold: true
            font.family: Theme.fontFamily
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Item { Layout.fillWidth: true }

    // Trailing actions slot via default property children of a Row
    default property alias actions: actionRow.data
    Row {
        id: actionRow
        spacing: 8
        Layout.alignment: Qt.AlignVCenter
    }
}
