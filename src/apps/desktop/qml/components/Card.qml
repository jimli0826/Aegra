import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

Rectangle {
    id: card
    property string title: ""
    property string actionText: ""
    property Component headerRightComponent: null
    signal actionClicked()

    // Soft subtle top-to-bottom gradient
    gradient: Gradient {
        orientation: Gradient.Vertical
        GradientStop { position: 0.0; color: Theme.colorCard }
        GradientStop { position: 1.0; color: Theme.colorCardEnd }
    }
    radius: Theme.radiusCard
    border.width: 1
    border.color: Theme.colorBorder

    // Very light subtle bottom-only shadow
    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 14
        anchors.bottomMargin: -4
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        radius: card.radius
        color: Theme.colorCardShadow
        opacity: 0.22
        visible: Theme.colorCardShadow.a > 0
    }

    // Do not use id "root" here — nested Component { onClicked: root.xxx }
    // would resolve to this Card instead of the page.

    RowLayout {
        id: headerRow
        // Keep header (and + Add) above page content for hit-testing.
        z: 10
        visible: card.title.length > 0 || card.headerRightComponent !== null
                 || card.actionText.length > 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 22
        anchors.rightMargin: 16
        anchors.topMargin: 16
        height: 32

        Text {
            visible: card.title.length > 0
            text: card.title
            color: Theme.colorTextWhite
            font.pixelSize: 16
            font.bold: true
            font.family: Theme.fontFamily
            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
        }

        Item { Layout.fillWidth: true }

        Loader {
            id: rightLoader
            active: card.headerRightComponent !== null
            sourceComponent: card.headerRightComponent
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
        }

        Text {
            visible: card.headerRightComponent === null && card.actionText.length > 0
            text: card.actionText
            color: Theme.colorAccentBlue
            font.pixelSize: 12
            font.bold: true
            font.family: Theme.fontFamily
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: card.actionClicked()
            }
        }
    }
}
