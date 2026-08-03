import QtQuick 2.15
import QtQuick.Controls 2.15
import ".."

Button {
    id: root
    property bool primary: false
    property bool danger: false

    implicitHeight: 32
    implicitWidth: contentItem.implicitWidth + 24

    contentItem: Text {
        text: root.text
        color: root.enabled ? Theme.colorTextWhite : Theme.colorButtonDisabledText
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.bold: root.primary
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: 4
        border.width: 1
        border.color: root.danger && root.enabled ? "#b64249" : Theme.colorBorder
        color: {
            if (!root.enabled)
                return Theme.colorButtonDisabled
            if (root.danger)
                return root.hovered ? "#cc3333" : "#a93d45"
            if (root.primary)
                return root.hovered ? "#3198d2" : "#267eae"
            return root.hovered ? Theme.colorButtonHover : Theme.colorButton
        }
    }
}
