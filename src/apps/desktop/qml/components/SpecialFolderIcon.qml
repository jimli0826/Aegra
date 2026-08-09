import QtQuick 2.15
import QtQuick.Layouts 1.15

/**
 * Compact glyphs for Explorer-style special folders on the file backup source tree.
 * variant: desktop | downloads | documents | pictures | music | videos
 */
Item {
    id: root
    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    property int size: 16
    property string variant: "documents"
    property real iconOpacity: 1.0

    opacity: iconOpacity
    Layout.preferredWidth: size
    Layout.preferredHeight: size
    Layout.minimumWidth: size
    Layout.minimumHeight: size
    Layout.maximumWidth: size
    Layout.maximumHeight: size
    Layout.alignment: Qt.AlignVCenter

    readonly property color accent: {
        switch (String(variant).toLowerCase()) {
        case "desktop": return "#3B82F6"
        case "downloads": return "#10B981"
        case "documents": return "#64748B"
        case "pictures": return "#38BDF8"
        case "music": return "#F97316"
        case "videos": return "#A855F7"
        default: return "#64748B"
        }
    }

    // Soft tile behind the glyph
    Rectangle {
        anchors.fill: parent
        radius: Math.max(2, size * 0.18)
        color: Qt.rgba(accent.r, accent.g, accent.b, 0.16)
        border.width: 1
        border.color: Qt.rgba(accent.r, accent.g, accent.b, 0.35)
    }

    // Desktop: monitor
    Item {
        visible: String(root.variant).toLowerCase() === "desktop"
        anchors.centerIn: parent
        width: size * 0.72
        height: size * 0.72
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            width: parent.width * 0.92
            height: parent.height * 0.62
            radius: 2
            color: root.accent
            Rectangle {
                anchors.fill: parent
                anchors.margins: Math.max(1, size * 0.08)
                radius: 1
                color: "#E0F2FE"
            }
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: parent.height * 0.08
            width: parent.width * 0.28
            height: parent.height * 0.12
            radius: 1
            color: root.accent
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            width: parent.width * 0.55
            height: Math.max(1, size * 0.06)
            radius: 1
            color: root.accent
        }
    }

    // Downloads: arrow into tray
    Item {
        visible: String(root.variant).toLowerCase() === "downloads"
        anchors.centerIn: parent
        width: size * 0.7
        height: size * 0.7
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            width: Math.max(2, size * 0.12)
            height: parent.height * 0.42
            radius: width / 2
            color: root.accent
        }
        // Chevron tip
        Canvas {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: parent.height * 0.28
            width: parent.width * 0.55
            height: parent.height * 0.32
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = root.accent
                ctx.beginPath()
                ctx.moveTo(0, 0)
                ctx.lineTo(width, 0)
                ctx.lineTo(width / 2, height)
                ctx.closePath()
                ctx.fill()
            }
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Component.onCompleted: requestPaint()
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Math.max(2, size * 0.1)
            radius: 1
            color: root.accent
        }
    }

    // Documents: page
    Item {
        visible: String(root.variant).toLowerCase() === "documents"
        anchors.centerIn: parent
        width: size * 0.58
        height: size * 0.72
        Rectangle {
            anchors.fill: parent
            radius: 2
            color: "#F8FAFC"
            border.width: 1
            border.color: root.accent
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: parent.height * 0.28
            anchors.leftMargin: parent.width * 0.18
            anchors.rightMargin: parent.width * 0.18
            height: Math.max(1, size * 0.06)
            color: root.accent
            opacity: 0.7
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: parent.height * 0.48
            anchors.leftMargin: parent.width * 0.18
            anchors.rightMargin: parent.width * 0.18
            height: Math.max(1, size * 0.06)
            color: root.accent
            opacity: 0.55
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: parent.height * 0.68
            anchors.leftMargin: parent.width * 0.18
            anchors.rightMargin: parent.width * 0.28
            height: Math.max(1, size * 0.06)
            color: root.accent
            opacity: 0.4
        }
    }

    // Pictures: landscape frame
    Item {
        visible: String(root.variant).toLowerCase() === "pictures"
        anchors.centerIn: parent
        width: size * 0.72
        height: size * 0.58
        Rectangle {
            anchors.fill: parent
            radius: 2
            color: "#E0F2FE"
            border.width: 1
            border.color: root.accent
        }
        // Sun
        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: parent.width * 0.12
            width: parent.width * 0.22
            height: width
            radius: width / 2
            color: "#FBBF24"
        }
        // Hill
        Canvas {
            anchors.fill: parent
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = root.accent
                ctx.beginPath()
                ctx.moveTo(0, height * 0.78)
                ctx.lineTo(width * 0.38, height * 0.42)
                ctx.lineTo(width * 0.62, height * 0.62)
                ctx.lineTo(width, height * 0.48)
                ctx.lineTo(width, height)
                ctx.lineTo(0, height)
                ctx.closePath()
                ctx.fill()
            }
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Component.onCompleted: requestPaint()
        }
    }

    // Music: note
    Item {
        visible: String(root.variant).toLowerCase() === "music"
        anchors.centerIn: parent
        width: size * 0.62
        height: size * 0.72
        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            width: Math.max(2, size * 0.12)
            height: parent.height * 0.62
            radius: width / 2
            color: root.accent
        }
        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            width: parent.width * 0.55
            height: Math.max(2, size * 0.1)
            radius: 1
            color: root.accent
        }
        Rectangle {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.bottomMargin: parent.height * 0.08
            width: parent.width * 0.42
            height: parent.width * 0.36
            radius: width / 2
            color: root.accent
        }
    }

    // Videos: play badge
    Item {
        visible: String(root.variant).toLowerCase() === "videos"
        anchors.centerIn: parent
        width: size * 0.72
        height: size * 0.58
        Rectangle {
            anchors.fill: parent
            radius: 3
            color: root.accent
        }
        Canvas {
            anchors.centerIn: parent
            width: parent.width * 0.42
            height: parent.height * 0.55
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = "#FFFFFF"
                ctx.beginPath()
                ctx.moveTo(0, 0)
                ctx.lineTo(width, height / 2)
                ctx.lineTo(0, height)
                ctx.closePath()
                ctx.fill()
            }
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Component.onCompleted: requestPaint()
        }
    }
}
