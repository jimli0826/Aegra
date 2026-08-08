import QtQuick 2.15
import QtQuick.Layouts 1.15

/**
 * Compact document glyph for file browse trees (light page + folded corner).
 */
Item {
    id: root
    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    property int size: 16
    property real iconOpacity: 1.0

    opacity: iconOpacity
    Layout.preferredWidth: size
    Layout.preferredHeight: size
    Layout.minimumWidth: size
    Layout.minimumHeight: size
    Layout.maximumWidth: size
    Layout.maximumHeight: size
    Layout.alignment: Qt.AlignVCenter

    Rectangle {
        id: page
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        width: size * 0.72
        height: size * 0.9
        radius: Math.max(1, size * 0.06)
        color: "#F4F7FB"
        border.width: 1
        border.color: "#A8B4C4"

        // Folded corner
        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            width: parent.width * 0.34
            height: parent.width * 0.34
            color: "#DDE5F0"
            border.width: 1
            border.color: "#A8B4C4"
            // Cover bottom/left edges so only the diagonal fold reads.
            Rectangle {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: parent.width + 1
                height: parent.height + 1
                anchors.leftMargin: -1
                anchors.bottomMargin: -1
                color: page.color
                transform: Rotation {
                    origin.x: 0
                    origin.y: 0
                    angle: 0
                }
            }
        }

        // Text lines
        Column {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: parent.width * 0.16
            anchors.rightMargin: parent.width * 0.16
            anchors.verticalCenterOffset: parent.height * 0.08
            spacing: Math.max(1, size * 0.08)
            Repeater {
                model: 3
                Rectangle {
                    width: parent.width * (index === 2 ? 0.55 : 1.0)
                    height: Math.max(1, size * 0.06)
                    radius: height / 2
                    color: "#B0BCCB"
                }
            }
        }
    }
}
