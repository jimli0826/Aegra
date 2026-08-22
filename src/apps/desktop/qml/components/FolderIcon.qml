import QtQuick 2.15
import QtQuick.Layouts 1.15

/**
 * Win11 Fluent-style folder glyph for file browse trees.
 * Flat two-tone: dark amber back flap + light yellow front panel.
 * No borders, gloss, or shadows; no emoji dependency.
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

    // Back sheet: tab silhouette on the top-left.
    Rectangle {
        anchors.left: body.left
        anchors.top: parent.top
        anchors.topMargin: size * 0.10
        width: size * 0.44
        height: size * 0.30
        radius: Math.max(1.5, size * 0.10)
        color: "#E8A33D"
    }
    Rectangle {
        id: backSheet
        anchors.left: body.left
        anchors.right: body.right
        anchors.top: parent.top
        anchors.topMargin: size * 0.20
        anchors.bottom: body.bottom
        radius: Math.max(2, size * 0.12)
        color: "#E8A33D"
    }

    // Front panel: light yellow, slightly shorter so a strip of the back shows.
    Rectangle {
        id: body
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: size * 0.08
        width: size * 0.92
        height: size * 0.56
        radius: Math.max(2, size * 0.12)
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#FFE29E" }
            GradientStop { position: 1.0; color: "#FFC44D" }
        }
    }
}
