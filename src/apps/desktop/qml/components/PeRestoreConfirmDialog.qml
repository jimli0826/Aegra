import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

// Irreversible-confirmation dialog for the WinPE offline system-disk restore
// (WINPE_OFFLINE_RESTORE design §11): shows the target identity, explains the
// reboot hand-off, and requires an explicit acknowledgement checkbox.
Popup {
    id: root
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, parent ? parent.width - 48 : 460)
    padding: 16

    property string targetText: ""

    signal accepted()
    signal cancelled()

    background: Rectangle {
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: Theme.colorCard }
            GradientStop { position: 1.0; color: Theme.colorCardEnd }
        }
        radius: 12
        border.width: 1
        border.color: Theme.colorBorder
    }

    onOpened: acknowledgeBox.checked = false

    contentItem: ColumnLayout {
        spacing: 12
        Text {
            Layout.fillWidth: true
            //% "Offline system-disk restore"
            text: qsTrId("aegra.restore.pe_confirm_title")
            color: Theme.colorTextWhite
            font.pixelSize: 14
            font.bold: true
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            //% "The system disk cannot be restored while Windows is running. Aegra will prepare a recovery environment; after you restart the computer, the restore runs before Windows starts."
            text: qsTrId("aegra.restore.pe_confirm_text")
            color: Theme.colorTextGrey
            font.pixelSize: 12
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            visible: root.targetText.length > 0
            //% "Target disk: %1"
            text: qsTrId("aegra.restore.pe_confirm_target").arg(root.targetText)
            color: Theme.colorTextWhite
            font.pixelSize: 12
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            CheckBox {
                id: acknowledgeBox
                Layout.alignment: Qt.AlignTop
            }
            Text {
                Layout.fillWidth: true
                //% "I understand that every byte on the target disk will be overwritten and this cannot be undone."
                text: qsTrId("aegra.restore.pe_confirm_ack")
                color: Theme.colorTextWhite
                font.pixelSize: 12
                font.family: Theme.fontFamily
                wrapMode: Text.WordWrap
                MouseArea {
                    anchors.fill: parent
                    onClicked: acknowledgeBox.checked = !acknowledgeBox.checked
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Item { Layout.fillWidth: true }
            AppButton {
                //% "Cancel"
                text: qsTrId("aegra.common.cancel")
                onClicked: {
                    root.cancelled()
                    root.close()
                }
            }
            AppButton {
                //% "Prepare offline restore"
                text: qsTrId("aegra.restore.pe_confirm_button")
                enabled: acknowledgeBox.checked
                onClicked: {
                    root.accepted()
                    root.close()
                }
            }
        }
    }
}
