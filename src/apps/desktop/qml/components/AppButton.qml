import QtQuick 2.15
import QtQuick.Controls 2.15
import ".."

// Matches backup/src/gui RepoButton / inline Button chrome.
Button {
    id: root
    property bool primary: false
    property bool danger: false
    property bool borderVisible: true

    implicitHeight: 32
    implicitWidth: Math.max(90, contentItem.implicitWidth + 24)
    padding: 12

    contentItem: Text {
        text: root.text
        color: {
            if (!root.enabled)
                return Theme.colorButtonDisabledText
            if (root.primary || root.danger)
                return Theme.colorOnAccent
            return Theme.colorTextWhite
        }
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.bold: root.primary || root.danger
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        opacity: root.enabled ? 1.0 : 0.85
    }

    background: Rectangle {
        radius: Theme.radiusButton
        border.width: (root.visualFocus || root.activeFocus) ? 2 : ((root.primary || root.danger || !root.borderVisible) ? 0 : 1)
        border.color: {
            if (!root.enabled)
                return Theme.colorBorder
            if (root.visualFocus || root.activeFocus)
                return Theme.colorAccentBlue
            if (root.danger)
                return root.hovered ? "#aa2222" : "#882222"
            return Theme.colorBorder
        }
        color: {
            if (!root.enabled)
                return Theme.colorButtonDisabled
            if (root.danger)
                return root.hovered ? "#e03333" : "#cc3333"
            if (root.primary)
                return (root.hovered || root.visualFocus || root.activeFocus) ? Theme.colorLinkHover : Theme.colorAccentBlue
            return (root.hovered || root.visualFocus || root.activeFocus) ? Theme.colorButtonHover : Theme.colorButton
        }
        opacity: root.enabled ? 1.0 : 0.55

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            cursorShape: Qt.PointingHandCursor
        }
    }
}
