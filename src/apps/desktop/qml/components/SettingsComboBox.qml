pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

ComboBox {
    id: root

    property bool useThemeLabels: false

    function itemLabel(item) {
        if (!item)
            return ""
        if (useThemeLabels)
            return Theme.themeLabel(item)
        return item.label ? item.label : ""
    }

    implicitHeight: 34
    hoverEnabled: true

    background: Rectangle {
        color: Theme.colorInput
        radius: Theme.radiusControl
        border.width: 1
        border.color: Theme.colorBorder
    }

    indicator: ComboBoxIndicator { combo: root }

    contentItem: RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 12
        anchors.rightMargin: 28
        spacing: 8

        Image {
            readonly property var selectedItem: root.currentIndex >= 0 && root.model
                                                ? root.model[root.currentIndex] : null
            Layout.preferredWidth: visible ? 18 : 0
            Layout.preferredHeight: 18
            source: selectedItem && selectedItem.iconSource ? selectedItem.iconSource : ""
            visible: source.toString().length > 0
            fillMode: Image.PreserveAspectFit
            smooth: true
        }

        Text {
            Layout.fillWidth: true
            text: root.currentIndex >= 0 && root.model
                  ? root.itemLabel(root.model[root.currentIndex]) : ""
            color: root.enabled ? Theme.colorTextWhite : Theme.colorTextDim
            font.pixelSize: 13
            font.family: Theme.fontFamily
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    popup: Popup {
        y: root.height + 2
        width: root.width
        padding: 4
        implicitHeight: Math.min(200, contentItem.implicitHeight + 8)
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
        }
        background: Rectangle {
            color: Theme.colorPopup
            border.color: Theme.colorBorder
            radius: Theme.radiusControl
        }
    }

    delegate: ItemDelegate {
        id: itemDelegate
        required property int index
        required property var modelData
        width: root.width - 8
        height: 32
        hoverEnabled: true
        highlighted: root.highlightedIndex === index
        contentItem: RowLayout {
            spacing: 8

            Image {
                Layout.leftMargin: 10
                Layout.preferredWidth: visible ? 18 : 0
                Layout.preferredHeight: 18
                source: itemDelegate.modelData.iconSource || ""
                visible: source.toString().length > 0
                fillMode: Image.PreserveAspectFit
                smooth: true
            }

            Text {
                Layout.fillWidth: true
                text: root.itemLabel(itemDelegate.modelData)
                color: Theme.colorTextWhite
                font.pixelSize: 13
                font.family: Theme.fontFamily
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
        }
        background: Rectangle {
            radius: Theme.radiusControl
            color: (itemDelegate.hovered || itemDelegate.highlighted)
                   ? Theme.colorHover : "transparent"
        }
    }
}
