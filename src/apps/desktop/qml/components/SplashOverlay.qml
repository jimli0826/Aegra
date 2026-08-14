import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import ".."

// Matches backup/src/gui SplashScreen: logo ring, indeterminate bar, Retry/Quit on error.
// Host window resizes to preferredWidth/Height (compact card, not full main UI).
Rectangle {
    id: root
    visible: !windowAppReady
    anchors.fill: parent
    radius: nativeWindowCorners ? 0 : Theme.radiusWindow
    color: Theme.colorCard
    z: 1000
    focus: visible
    Accessible.name: serviceClient.splashStatusText
    Accessible.role: Accessible.Pane

    /// When true, splash is dismissed (host sets this after first ready).
    property bool windowAppReady: false

    // Prefer explicit error text + not-busy so we do not flicker when busy toggles mid-frame.
    readonly property bool hasError: !windowAppReady
                                     && !serviceClient.splashBusy
                                     && serviceClient.splashErrorText.length > 0
    readonly property string serviceEndpoint: "\\\\.\\pipe\\aegra-service-control"

    /// Preferred size of the splash window (host resizes to this) — same as old SplashScreen.
    readonly property int preferredWidth: hasError ? 680 : 600
    readonly property int preferredHeight: hasError ? 560 : 440

    signal sizeHintChanged()
    signal finished()
    signal quitRequested()

    onHasErrorChanged: sizeHintChanged()
    onPreferredWidthChanged: sizeHintChanged()
    onPreferredHeightChanged: sizeHintChanged()

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: 1
        border.color: Theme.colorBorder
    }

    // Drag splash window by top area
    MouseArea {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 36
        z: 2
        onPressed: {
            var w = Window.window
            if (w)
                w.startSystemMove()
        }
    }

    // Close (self-drawn Canvas control)
    WindowButton {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 4
        anchors.rightMargin: 4
        z: 3
        role: "close"
        onClicked: root.quitRequested()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 40
        spacing: 24

        Item { Layout.fillHeight: true }

        // Logo ring
        Item {
            Layout.alignment: Qt.AlignHCenter
            width: 104
            height: 104

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: "transparent"
                border.width: 3
                border.color: Theme.colorAccentBlue
                opacity: 0.35
            }

            Item {
                id: spinner
                anchors.fill: parent

                Repeater {
                    model: 8
                    delegate: Rectangle {
                        required property int index
                        width: 10
                        height: 10
                        radius: 5
                        color: Theme.colorAccentBlue
                        opacity: 0.25 + (index / 8.0) * 0.75
                        x: spinner.width / 2 - width / 2
                           + Math.cos((index / 8.0) * 2 * Math.PI - Math.PI / 2) * 42
                        y: spinner.height / 2 - height / 2
                           + Math.sin((index / 8.0) * 2 * Math.PI - Math.PI / 2) * 42
                    }
                }

                RotationAnimation on rotation {
                    from: 0
                    to: 360
                    duration: 1400
                    loops: Animation.Infinite
                    running: !root.hasError && root.visible && root.opacity > 0.05
                }
            }

            Rectangle {
                anchors.centerIn: parent
                width: 60
                height: 60
                radius: 12
                color: Theme.colorBg
                border.width: 1
                border.color: Theme.colorBorder
                clip: true

                Image {
                    id: splashLogo
                    anchors.centerIn: parent
                    width: 40
                    height: 40
                    source: "qrc:/Aegra/icons/product_64.png"
                    sourceSize.width: 64
                    sourceSize.height: 64
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    mipmap: true
                    onStatusChanged: {
                        if (status === Image.Error
                                && source.toString().indexOf("product_64") >= 0)
                            source = "qrc:/Aegra/icons/product.png"
                    }

                    SequentialAnimation on scale {
                        running: !root.hasError && root.visible
                        loops: Animation.Infinite
                        NumberAnimation {
                            from: 1.0
                            to: 1.08
                            duration: 700
                            easing.type: Easing.InOutQuad
                        }
                        NumberAnimation {
                            from: 1.08
                            to: 1.0
                            duration: 700
                            easing.type: Easing.InOutQuad
                        }
                    }
                }
            }
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            //% "Aegra"
            text: qsTrId("aegra.app.title")
            color: Theme.colorTextWhite
            font.pixelSize: 28
            font.bold: true
            font.family: Theme.fontFamily
        }

        // Primary status (red on error, grey when loading)
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.hasError
                  ? qsTrId("aegra.splash.status.failed")
                  : serviceClient.splashStatusText
            color: root.hasError ? Theme.colorAccentRed : Theme.colorTextGrey
            font.pixelSize: 16
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
        }

        // Error detail block (old: ensure service + server + reason)
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            visible: root.hasError
            text: {
                //% "Please ensure Aegra Service is running and try again.\nServer: %1"
                var base = qsTrId("aegra.splash.error.detail").arg(root.serviceEndpoint)
                var detail = serviceClient.splashErrorText || ""
                if (detail.length > 0)
                    return base + "\n" + detail
                return base
            }
            color: Theme.colorTextGrey
            font.pixelSize: 13
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
            lineHeight: 1.25
        }

        // Indeterminate progress bar (loading only)
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 240
            Layout.preferredHeight: 6
            radius: 3
            color: "#333"
            visible: !root.hasError
            clip: true

            Rectangle {
                width: 72
                height: parent.height
                radius: 3
                color: Theme.colorAccentBlue

                SequentialAnimation on x {
                    running: !root.hasError && root.visible
                    loops: Animation.Infinite
                    NumberAnimation {
                        from: -72
                        to: 240
                        duration: 1100
                        easing.type: Easing.InOutCubic
                    }
                }
            }
        }

        // Retry + Quit
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 16
            visible: root.hasError

            Button {
                //% "Retry"
                text: qsTrId("aegra.common.retry")
                Layout.preferredWidth: 160
                Layout.preferredHeight: 44
                background: Rectangle {
                    color: parent.hovered ? Theme.colorButtonHover : Theme.colorButton
                    radius: 6
                }
                contentItem: Text {
                    text: parent.text
                    color: Theme.colorTextWhite
                    font.pixelSize: 15
                    font.family: Theme.fontFamily
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: serviceClient.reconnect()
            }

            Button {
                //% "Quit"
                text: qsTrId("aegra.common.quit")
                Layout.preferredWidth: 128
                Layout.preferredHeight: 44
                background: Rectangle {
                    color: parent.hovered ? "#444" : "#3a3a3a"
                    radius: 6
                    border.width: 1
                    border.color: Theme.colorBorder
                }
                contentItem: Text {
                    text: parent.text
                    color: Theme.colorTextWhite
                    font.pixelSize: 15
                    font.family: Theme.fontFamily
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: root.quitRequested()
            }
        }

        Item { Layout.fillHeight: true }
    }
}
