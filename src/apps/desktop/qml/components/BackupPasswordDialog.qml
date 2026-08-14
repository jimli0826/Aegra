import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

// Modal password prompt for encrypted backup archives (old Restore/Mount password UX).
Popup {
    id: root
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: Overlay.overlay
    width: Math.min(360, parent ? parent.width - 48 : 360)
    padding: 16

    property string titleText: qsTrId("aegra.restore.password_title")
    property string hintText: qsTrId("aegra.restore.password_hint")
    property string errorText: ""
    property int passwordMaxLength: 32

    signal accepted(string password)
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

    onOpened: {
        passwordField.text = ""
        passwordField.forceActiveFocus()
    }

    contentItem: ColumnLayout {
        spacing: 12
        Text {
            Layout.fillWidth: true
            text: root.titleText
            color: Theme.colorTextWhite
            font.pixelSize: 14
            font.bold: true
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            text: root.hintText
            color: Theme.colorTextGrey
            font.pixelSize: 12
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            visible: root.errorText.length > 0
            text: root.errorText
            color: Theme.colorAccentRed
            font.pixelSize: 12
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Theme.colorInput
            radius: 4
            border.width: 1
            border.color: passwordField.activeFocus ? Theme.colorAccentBlue : Theme.colorBorder
            TextInput {
                id: passwordField
                anchors.fill: parent
                anchors.margins: 8
                color: Theme.colorTextWhite
                font.pixelSize: 13
                font.family: Theme.fontFamily
                echoMode: TextInput.Password
                maximumLength: root.passwordMaxLength
                selectByMouse: true
                activeFocusOnTab: true
                Keys.onReturnPressed: root.submit()
                Keys.onEnterPressed: root.submit()
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
                //% "OK"
                text: qsTrId("aegra.common.ok")
                onClicked: root.submit()
            }
        }
    }

    function submit() {
        var p = passwordField.text || ""
        if (p.length === 0) {
            root.errorText = qsTrId("aegra.backup.opt.password_required")
            return
        }
        root.errorText = ""
        root.accepted(p)
        root.close()
    }
}
