import QtQuick 2.15
import QtQuick.Controls 2.15
import ".."

// Matches backup/src/gui RepoButton / inline Button chrome.
Button {
    id: root
    property bool primary: false
    property bool danger: false

    implicitHeight: 32
    implicitWidth: Math.max(90, contentItem.implicitWidth + 24)
    padding: 12

    contentItem: Text {
        text: root.text
        color: Theme.colorTextWhite
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.bold: root.primary || root.danger
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        opacity: root.enabled ? 1.0 : 0.85
    }

    background: Rectangle {
        radius: 4
        border.width: 1
        border.color: {
            if (!root.enabled)
                return Theme.colorBorder
            if (root.danger)
                return root.hovered ? "#aa2222" : "#882222"
            return Theme.colorBorder
        }
        color: {
            if (!root.enabled)
                return Theme.colorInput
            if (root.danger)
                return root.hovered ? "#e03333" : "#cc3333"
            if (root.primary)
                return root.hovered ? Theme.colorButtonHover : Theme.colorAccentBlue
            return root.hovered ? Theme.colorButtonHover : Theme.colorButton
        }
        opacity: root.enabled ? 1.0 : 0.45
    }
}
