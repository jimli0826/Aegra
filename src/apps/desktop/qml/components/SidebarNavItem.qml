import QtQuick 2.15
import QtQuick.Controls 2.15
import ".."

// Single nav row — hover/active matches docs/design/index.html .nav-item
Item {
    id: nav
    property bool active: false
    /// Prefer not to shadow Item.enabled; use itemEnabled for click/hover gate.
    property bool itemEnabled: true
    property bool hovered: mouse.containsMouse && nav.itemEnabled
    property string iconName: "home"
    property string label: ""
    property bool showLabel: true
    property color idleColor: Theme.colorMenuIdle
    property color hoverTextColor: Theme.colorMenuHoverText
    property color hoverBgColor: Theme.colorMenuHoverBg
    property color activeTextColor: Theme.colorMenuActiveText
    signal activated()

    // index.html: translateX(4px) on hover for ALL items (including active), press scale(0.97)
    readonly property real shiftX: nav.hovered ? 4 : 0
    readonly property real pressScale: (mouse.pressed && nav.itemEnabled) ? 0.97 : 1.0

    readonly property color washColor: {
        if (nav.active)
            return "transparent"
        if (nav.hovered)
            return nav.hoverBgColor
        return "transparent"
    }
    readonly property color fg: {
        if (nav.active)
            return nav.activeTextColor
        if (nav.hovered)
            return nav.hoverTextColor
        return nav.idleColor
    }

    height: 48
    opacity: nav.itemEnabled ? 1.0 : 0.4

    // MouseArea at outer root item level with z: 100 so it sits strictly on top of container and stays static
    MouseArea {
        id: mouse
        anchors.fill: parent
        z: 100
        enabled: nav.itemEnabled
        hoverEnabled: true
        cursorShape: nav.itemEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: nav.activated()
    }

    // Wrapper item that handles hover translate & press scale for pill + shadow together
    Item {
        id: container
        anchors.fill: parent
        z: 0

        transform: Translate {
            x: nav.shiftX
            Behavior on x {
                NumberAnimation {
                    duration: 250
                    easing.type: Easing.OutCubic
                }
            }
        }

        scale: nav.pressScale
        Behavior on scale {
            NumberAnimation {
                duration: 150
                easing.type: Easing.OutCubic
            }
        }

        // Main pill shape
        Rectangle {
            id: pill
            anchors.fill: parent
            radius: Theme.radiusMenu
            z: 1
            color: nav.washColor

            // Active gradient overlay matching CoachPro design (with hover highlight)
            Rectangle {
                anchors.fill: parent
                radius: parent.radius
                opacity: nav.active ? 1 : 0
                Behavior on opacity {
                    NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
                }
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: nav.hovered ? Qt.lighter(Theme.colorMenuActive, 1.1) : Theme.colorMenuActive }
                    GradientStop { position: 1.0; color: nav.hovered ? Qt.lighter(Theme.colorMenuActiveEnd, 1.1) : Theme.colorMenuActiveEnd }
                }
            }

            Row {
                visible: nav.showLabel
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14
                Item {
                    width: 20
                    height: 20
                    anchors.verticalCenter: parent.verticalCenter
                    NavIcon {
                        anchors.centerIn: parent
                        width: 20
                        height: 20
                        name: nav.iconName
                        color: nav.fg
                        strokeWidth: 2.0
                    }
                }
                Text {
                    text: nav.label
                    font.pixelSize: 14
                    font.family: Theme.fontFamily
                    font.weight: Font.DemiBold
                    color: nav.fg
                    anchors.verticalCenter: parent.verticalCenter
                    Behavior on color {
                        ColorAnimation { duration: 250; easing.type: Easing.OutCubic }
                    }
                }
            }

            NavIcon {
                visible: !nav.showLabel
                anchors.centerIn: parent
                width: 20
                height: 20
                name: nav.iconName
                color: nav.fg
                strokeWidth: 2.0
            }
        }
    }
}
