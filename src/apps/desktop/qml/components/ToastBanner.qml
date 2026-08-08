import QtQuick 2.15
import QtQuick.Layouts 1.15
import ".."

// Top drop-down toast matching backup/src/gui ToastBanner (success/error accents).
Item {
    id: root
    anchors.left: parent ? parent.left : undefined
    anchors.right: parent ? parent.right : undefined
    anchors.top: parent ? parent.top : undefined
    height: banner.height + 12
    z: 8000
    visible: serviceClient.toastVisible
    clip: false
    Accessible.name: serviceClient.toastText
    Accessible.role: Accessible.Notification

    Rectangle {
        id: banner
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 48, Math.max(280, msgText.implicitWidth + 56))
        height: Math.max(40, msgText.implicitHeight + 20)
        radius: Theme.radiusControl
        y: serviceClient.toastVisible ? 12 : -height - 20
        opacity: serviceClient.toastVisible ? 1 : 0
        border.width: 1
        border.color: serviceClient.toastIsError ? Theme.colorToastErrorBorder
                                                : Theme.colorToastSuccessBorder
        color: serviceClient.toastIsError ? Theme.colorToastErrorBg
                                          : Theme.colorToastSuccessBg

        Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: 180 } }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 12
            anchors.topMargin: 8
            anchors.bottomMargin: 8
            spacing: 10

            Rectangle {
                width: 4
                Layout.fillHeight: true
                radius: 2
                color: serviceClient.toastIsError ? Theme.colorToastErrorBorder
                                                 : Theme.colorToastSuccessBorder
            }

            Text {
                id: msgText
                Layout.fillWidth: true
                text: serviceClient.toastText
                color: Theme.colorTextWhite
                font.pixelSize: 13
                font.family: Theme.fontFamily
                wrapMode: Text.WordWrap
                maximumLineCount: 4
                elide: Text.ElideRight
            }

            Text {
                text: "\u2715"
                color: Theme.colorTextGrey
                font.pixelSize: 14
                font.family: Theme.fontFamily
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -8
                    cursorShape: Qt.PointingHandCursor
                    //% "Dismiss"
                    Accessible.name: qsTrId("aegra.common.dismiss")
                    onClicked: serviceClient.dismissToast()
                }
            }
        }
    }
}
