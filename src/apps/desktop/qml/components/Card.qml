import QtQuick 2.15
import QtQuick.Layouts 1.15
import ".."

Rectangle {
    id: root
    property string title: ""
    property string actionText: ""
    signal actionClicked()

    // Pure white background matching CoachPro design mockup
    color: "#ffffff"
    radius: 20
    border.width: 1
    border.color: "#ebf3f2"

    RowLayout {
        id: headerRow
        visible: root.title.length > 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 22
        anchors.rightMargin: 22
        anchors.topMargin: 20
        height: 24

        Text {
            text: root.title
            color: "#111111"
            font.pixelSize: 16
            font.bold: true
            font.family: Theme.fontFamily
            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
        }

        Item { Layout.fillWidth: true }

        Text {
            visible: root.actionText.length > 0
            text: root.actionText
            color: "#2A7982"
            font.pixelSize: 12
            font.bold: true
            font.family: Theme.fontFamily
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.actionClicked()
            }
        }
    }
}
