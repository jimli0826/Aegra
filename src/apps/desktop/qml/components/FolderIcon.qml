import QtQuick 2.15
import QtQuick.Layouts 1.15

/**
 * Compact Win11-style folder glyph for file browse trees.
 * Soft yellow body + raised tab; no emoji dependency.
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

    // Soft drop shadow
    Rectangle {
        anchors.horizontalCenter: body.horizontalCenter
        anchors.top: body.bottom
        anchors.topMargin: -1
        width: body.width * 0.9
        height: Math.max(2, size * 0.08)
        radius: height / 2
        color: "#28000000"
    }

    // Tab (back flap)
    Rectangle {
        id: tab
        anchors.left: body.left
        anchors.bottom: body.top
        anchors.bottomMargin: -size * 0.08
        width: size * 0.42
        height: size * 0.28
        radius: Math.max(2, size * 0.08)
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#FFE08A" }
            GradientStop { position: 1.0; color: "#F0C14D" }
        }
        border.width: 1
        border.color: "#D4A017"
    }

    // Folder body
    Rectangle {
        id: body
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: size * 0.08
        width: size * 0.92
        height: size * 0.68
        radius: Math.max(2, size * 0.12)
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#FFD966" }
            GradientStop { position: 0.55; color: "#F5C542" }
            GradientStop { position: 1.0; color: "#E0A820" }
        }
        border.width: 1
        border.color: "#C99212"

        // Top sheen
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: parent.height * 0.38
            radius: parent.radius
            color: "#55ffffff"
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: parent.height * 0.5
                color: parent.color
            }
        }

        // Front lip highlight
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: parent.height * 0.22
            anchors.leftMargin: parent.width * 0.06
            anchors.rightMargin: parent.width * 0.06
            height: 1
            color: "#40ffffff"
        }
    }
}
