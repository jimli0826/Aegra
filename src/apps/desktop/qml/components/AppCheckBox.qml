import QtQuick 2.15
import QtQuick.Controls 2.15
import ".."

// Standard CheckBox matching Aegra desktop theme design system.
CheckBox {
    id: root

    focusPolicy: Qt.TabFocus
    font.pixelSize: 12
    font.family: Theme.fontFamily
    spacing: 10
    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0

    indicator: Rectangle {
        implicitWidth: 18
        implicitHeight: 18
        x: root.leftPadding
        // Align with top text line when multi-line, or vertically center when single line
        y: (root.height > 24) ? (root.topPadding + 1) : Math.round(parent.height / 2 - height / 2)
        radius: 3
        color: {
            if (!root.enabled)
                return Theme.colorButtonDisabled
            return root.checked ? Theme.colorAccentBlue : Theme.colorInput
        }
        border.color: {
            if (!root.enabled)
                return Theme.colorBorder
            if (root.visualFocus)
                return Theme.colorAccentBlue
            return root.checked ? Theme.colorAccentBlue : Theme.colorBorder
        }
        border.width: 1

        Text {
            anchors.centerIn: parent
            text: root.checkState === Qt.PartiallyChecked ? "\u2212" : (root.checked ? "\u2713" : "")
            color: Theme.colorOnAccent
            font.pixelSize: 12
            font.bold: true
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
    }

    contentItem: Text {
        text: root.text
        font: root.font
        color: root.enabled ? Theme.colorTextWhite : Theme.colorButtonDisabledText
        leftPadding: root.indicator.width + root.spacing
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.WordWrap

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
    }
}
