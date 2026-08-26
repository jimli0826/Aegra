import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

// Confirm replacing a pending WinPE hand-off that has not been consumed
// (user prepared once and has not restarted yet).
Popup {
    id: root
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: Overlay.overlay
    width: Math.min(480, parent ? parent.width - 48 : 480)
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

    contentItem: ColumnLayout {
        spacing: 12
        Text {
            Layout.fillWidth: true
            //% "Replace pending offline restore?"
            text: qsTrId("aegra.restore.pe_replace_title")
            color: Theme.colorTextWhite
            font.pixelSize: 14
            font.bold: true
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            //% "An offline restore is already prepared and waiting for restart. Continuing will replace that preparation."
            text: qsTrId("aegra.restore.pe_replace_text")
            color: Theme.colorTextGrey
            font.pixelSize: 12
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            visible: root.targetText.length > 0
            //% "Pending target: %1"
            text: qsTrId("aegra.restore.pe_replace_target").arg(root.targetText)
            color: Theme.colorTextWhite
            font.pixelSize: 12
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
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
                //% "Replace and continue"
                text: qsTrId("aegra.restore.pe_replace_button")
                primary: true
                onClicked: {
                    root.accepted()
                    root.close()
                }
            }
        }
    }
}
