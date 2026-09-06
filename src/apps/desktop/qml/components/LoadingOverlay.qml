import QtQuick 2.15
import ".."

/**
 * Full-window blocking loading layer (old AegraImage LoadingOverlay).
 * Parent sets visible when ServiceClient.globalLoading after main UI is ready.
 */
Rectangle {
    id: root
    color: root.dimBackground ? Theme.colorScrim : "transparent"
    visible: false
    z: 1000
    opacity: visible ? 1 : 0
    Behavior on opacity {
        NumberAnimation { duration: 120 }
    }

    property string message: ""
    property bool actionsVisible: false
    property bool dimBackground: true
    signal quitRequested()
    signal diagnoseRequested()

    //% "Loading"
    Accessible.name: message.length > 0 ? message : qsTrId("aegra.common.loading")
    Accessible.role: Accessible.Pane

    // Block interaction underneath only for operations represented by globalLoading.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        preventStealing: true
        onPressed: function(mouse) { mouse.accepted = true }
        onReleased: function(mouse) { mouse.accepted = true }
        onClicked: function(mouse) { mouse.accepted = true }
        onDoubleClicked: function(mouse) { mouse.accepted = true }
        onWheel: function(wheel) { wheel.accepted = true }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(root.width - 80,
                        Math.max(root.actionsVisible ? 420 : 160,
                                 msgText.implicitWidth + 48))
        height: root.actionsVisible ? 178 : 120
        radius: Theme.radiusCard
        color: Theme.colorCard
        border.width: 1
        border.color: Theme.colorBorder

        Column {
            anchors.centerIn: parent
            width: parent.width - 48
            spacing: 14

            Item {
                id: spinner
                width: 44
                height: 44
                anchors.horizontalCenter: parent.horizontalCenter

                Repeater {
                    model: 8
                    delegate: Rectangle {
                        width: 7
                        height: 7
                        radius: 3.5
                        color: Theme.colorAccentBlue
                        opacity: 0.3 + (index / 8.0) * 0.7
                        x: spinner.width / 2 - width / 2
                           + Math.cos((index / 8.0) * 2 * Math.PI - Math.PI / 2) * 16
                        y: spinner.height / 2 - height / 2
                           + Math.sin((index / 8.0) * 2 * Math.PI - Math.PI / 2) * 16
                    }
                }

                RotationAnimation on rotation {
                    from: 0
                    to: 360
                    duration: 900
                    loops: Animation.Infinite
                    running: root.visible
                }
            }

            Text {
                id: msgText
                width: parent.width
                //% "Loading"
                text: root.message.length > 0 ? root.message : qsTrId("aegra.common.loading")
                color: Theme.colorTextWhite
                font.pixelSize: 13
                font.family: Theme.fontFamily
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12
                visible: root.actionsVisible

                AppButton {
                    width: 120
                    //% "Quit"
                    text: qsTrId("aegra.common.quit")
                    onClicked: root.quitRequested()
                }

                AppButton {
                    width: 120
                    primary: true
                    //% "Diagnose"
                    text: qsTrId("aegra.service.diagnose")
                    onClicked: root.diagnoseRequested()
                }
            }
        }
    }
}
