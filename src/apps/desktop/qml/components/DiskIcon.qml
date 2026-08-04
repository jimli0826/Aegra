import QtQuick 2.15
import QtQuick.Layouts 1.15

/**
 * Compact HDD icon without dark front panel.
 * variant: "hdd" | "system" | "optical" | "empty"
 */
Item {
    id: root
    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    property int size: 32
    property string variant: "hdd"
    property real iconOpacity: 1.0

    opacity: iconOpacity
    Layout.preferredWidth: size
    Layout.preferredHeight: size
    Layout.minimumWidth: size
    Layout.minimumHeight: size
    Layout.maximumWidth: size
    Layout.maximumHeight: size
    Layout.alignment: Qt.AlignVCenter

    readonly property bool isEmpty: variant === "empty"
    readonly property bool isSystem: variant === "system"
    readonly property bool isOptical: variant === "optical"

    // Soft shadow only (no black body)
    Rectangle {
        anchors.horizontalCenter: body.horizontalCenter
        anchors.top: body.bottom
        anchors.topMargin: -1
        width: body.width * 0.85
        height: Math.max(2, size * 0.08)
        radius: height / 2
        color: "#35000000"
    }

    // Single silver drive plate (no dark lower face)
    Rectangle {
        id: body
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: size * 0.12
        width: size * 0.86
        height: size * 0.42
        radius: Math.max(3, size * 0.1)
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: root.isEmpty ? "#eef0f3" : "#e4e7ec"
            }
            GradientStop {
                position: 0.45
                color: root.isEmpty ? "#c8ced6" : "#b8bfc8"
            }
            GradientStop {
                position: 1.0
                color: root.isEmpty ? "#a8b0ba" : "#949ca8"
            }
        }
        border.width: 1
        border.color: root.isEmpty ? "#a0a8b2" : "#7a828c"

        // top sheen
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: parent.height * 0.4
            radius: parent.radius
            color: "#60ffffff"
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: parent.height * 0.45
                color: parent.color
            }
        }

        // subtle mid line
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: parent.width * 0.1
            anchors.rightMargin: parent.width * 0.22
            height: 1
            color: "#28ffffff"
        }

        // Green LED on the plate (not on a black front)
        Rectangle {
            visible: !root.isEmpty
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: parent.width * 0.1
            width: Math.max(4, size * 0.12)
            height: width
            radius: width / 2
            color: "#3ddc84"
            border.width: 1
            border.color: "#301b8a4a"
            Rectangle {
                anchors.centerIn: parent
                width: parent.width * 1.65
                height: parent.height * 1.65
                radius: width / 2
                color: "#383ddc84"
                z: -1
            }
        }
    }

    // System: Windows 4-color logo above plate
    Item {
        visible: root.isSystem
        anchors.horizontalCenter: body.horizontalCenter
        anchors.bottom: body.top
        anchors.bottomMargin: 1
        width: size * 0.34
        height: size * 0.34
        Grid {
            anchors.centerIn: parent
            columns: 2
            spacing: Math.max(1, size * 0.03)
            Repeater {
                model: ["#f25022", "#7fba00", "#00a4ef", "#ffb900"]
                Rectangle {
                    width: size * 0.12
                    height: size * 0.12
                    color: modelData
                    radius: 1
                }
            }
        }
    }

    // Optical: disc above plate
    Item {
        visible: root.isOptical
        anchors.horizontalCenter: body.horizontalCenter
        anchors.bottom: body.top
        anchors.bottomMargin: -size * 0.02
        width: size * 0.4
        height: size * 0.4

        Rectangle {
            anchors.centerIn: parent
            width: parent.width
            height: parent.height
            radius: width / 2
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#f5f7fa" }
                GradientStop { position: 0.5; color: "#b0bec5" }
                GradientStop { position: 1.0; color: "#90a4ae" }
            }
            border.width: 1
            border.color: "#78909c"
            Rectangle {
                anchors.centerIn: parent
                width: parent.width * 0.22
                height: parent.height * 0.22
                radius: width / 2
                color: "#3a3e45"
                border.width: 1
                border.color: "#546e7a"
            }
        }
    }
}
