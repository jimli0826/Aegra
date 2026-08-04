import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

// Card-colored splash like backup/src/gui SplashScreen (no pure black frame).
Rectangle {
    id: root
    visible: serviceClient.splashVisible
    anchors.fill: parent
    color: Theme.colorCard
    z: 1000
    focus: visible
    Accessible.name: serviceClient.splashStatusText
    Accessible.role: Accessible.Pane

    Rectangle {
        anchors.fill: parent
        anchors.margins: 0
        color: "transparent"
        border.width: 1
        border.color: Theme.colorBorder
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 420)
        spacing: 16

        Image {
            Layout.alignment: Qt.AlignHCenter
            source: "qrc:/Aegra/icons/product_64.png"
            sourceSize.width: 64
            sourceSize.height: 64
            Layout.preferredWidth: 64
            Layout.preferredHeight: 64
            smooth: true
        }

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            //% "Aegra"
            text: qsTrId("aegra.app.title")
            color: Theme.colorTextWhite
            font.family: Theme.fontFamily
            font.pixelSize: 22
            font.bold: true
        }

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: serviceClient.splashStatusText
            color: Theme.colorTextGrey
            font.family: Theme.fontFamily
            font.pixelSize: 14
        }

        BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: serviceClient.splashBusy
            visible: running
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
        }

        Text {
            Layout.fillWidth: true
            visible: serviceClient.splashErrorText.length > 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: serviceClient.splashErrorText
            color: Theme.colorAccentRed
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }

        AppButton {
            Layout.alignment: Qt.AlignHCenter
            visible: !serviceClient.splashBusy && serviceClient.splashVisible
            //% "Retry"
            text: qsTrId("aegra.common.retry")
            primary: true
            enabled: !serviceClient.splashBusy
            onClicked: serviceClient.reconnect()
            Accessible.name: text
        }
    }
}
