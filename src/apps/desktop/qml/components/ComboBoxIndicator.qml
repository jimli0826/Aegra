import QtQuick 2.15
import ".."

// Shared ComboBox chevron — backup/src/gui ComboBoxIndicator.
Text {
    id: root
    property Item combo: null

    x: combo ? (combo.width - width - 10) : 0
    y: combo ? ((combo.height - height) / 2) : 0
    width: 14
    horizontalAlignment: Text.AlignHCenter
    text: (combo && combo.popup && combo.popup.visible) ? "\u25B2" : "\u25BC"
    color: {
        if (!combo || !combo.enabled)
            return Theme.colorTextDim
        return Theme.colorTextWhite
    }
    font.pixelSize: 10
    font.bold: true
    font.family: Theme.fontFamily
    opacity: {
        if (!combo)
            return 0.9
        if (combo.hovered || (combo.popup && combo.popup.visible))
            return 1.0
        return 0.9
    }
}
