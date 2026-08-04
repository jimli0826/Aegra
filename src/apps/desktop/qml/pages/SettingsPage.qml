import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Visual baseline: backup/src/gui SettingsPage as right drawer content.
Item {
    id: root
    signal closeRequested()
    //% "Settings"
    Accessible.name: qsTrId("aegra.nav.settings")

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: mainCol.implicitHeight + 40
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: mainCol
            width: parent.width
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 20
            spacing: 20

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Rectangle {
                    width: 3
                    height: 20
                    color: Theme.colorAccentBlue
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    //% "Settings"
                    text: qsTrId("aegra.nav.settings")
                    color: Theme.colorTextWhite
                    font.pixelSize: 18
                    font.bold: true
                    font.family: Theme.fontFamily
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                }
                Button {
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 28
                    text: "\u2715"
                    background: Rectangle {
                        color: parent.hovered ? Theme.colorButtonHover : "transparent"
                        radius: 4
                    }
                    contentItem: Text {
                        text: parent.text
                        color: Theme.colorTextWhite
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: root.closeRequested()
                }
            }

            Card {
                Layout.fillWidth: true
                Layout.preferredHeight: languageColumn.implicitHeight + 70
                //% "Language"
                title: qsTrId("aegra.settings.language")

                ColumnLayout {
                    id: languageColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: 50
                    anchors.leftMargin: 20
                    anchors.rightMargin: 20
                    anchors.bottomMargin: 16
                    spacing: 12

                    Text {
                        Layout.fillWidth: true
                        //% "Choose the display language for the desktop client"
                        text: qsTrId("aegra.settings.language_desc")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                    }

                    ComboBox {
                        id: languageCombo
                        Layout.preferredWidth: 280
                        Layout.preferredHeight: 36
                        model: localeController.availableLanguages
                        textRole: "label"
                        currentIndex: {
                            const tags = localeController.availableLanguages
                            for (let i = 0; i < tags.length; ++i) {
                                if (tags[i].tag === localeController.language)
                                    return i
                            }
                            return 0
                        }
                        onActivated: function(index) {
                            const languages = localeController.availableLanguages
                            if (index >= 0 && index < languages.length)
                                localeController.setLanguage(languages[index].tag)
                        }
                        background: Rectangle {
                            color: Theme.colorInput
                            radius: 4
                            border.width: 1
                            border.color: languageCombo.activeFocus || languageCombo.popup.visible
                                          ? Theme.colorAccentBlue : Theme.colorBorder
                        }
                        contentItem: Text {
                            leftPadding: 12
                            rightPadding: 24
                            text: languageCombo.displayText
                            color: Theme.colorTextWhite
                            font.pixelSize: 14
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        popup: Popup {
                            y: languageCombo.height + 2
                            width: languageCombo.width
                            implicitHeight: contentItem.implicitHeight
                            padding: 1
                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                model: languageCombo.popup.visible
                                       ? languageCombo.delegateModel : null
                                currentIndex: languageCombo.highlightedIndex
                            }
                            background: Rectangle {
                                color: Theme.colorPopup
                                border.color: Theme.colorBorder
                                radius: 4
                            }
                        }
                        delegate: ItemDelegate {
                            width: languageCombo.width
                            height: 32
                            contentItem: Text {
                                text: modelData.label
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                color: parent.highlighted ? Theme.colorHover : "transparent"
                            }
                        }
                    }
                }
            }

            Card {
                Layout.fillWidth: true
                Layout.preferredHeight: themeColumn.implicitHeight + 70
                //% "Theme"
                title: qsTrId("aegra.settings.theme")

                ColumnLayout {
                    id: themeColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: 50
                    anchors.leftMargin: 20
                    anchors.rightMargin: 20
                    anchors.bottomMargin: 16
                    spacing: 12

                    Text {
                        Layout.fillWidth: true
                        //% "Appearance follows the blueExtra palette until theme Service settings are connected"
                        text: qsTrId("aegra.settings.theme_desc")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: 12

                        Repeater {
                            model: [
                                {
                                    id: "blueExtra",
                                    label: qsTrId("aegra.settings.theme.blue_extra"),
                                    previewBg: "#1b2a40",
                                    previewCard: "#2a3f5c",
                                    previewAccent: "#4fc1ff",
                                    previewText: "#ffffff",
                                    selected: true
                                },
                                {
                                    id: "dark",
                                    label: qsTrId("aegra.settings.theme.dark"),
                                    previewBg: "#1e1e1e",
                                    previewCard: "#2d2d30",
                                    previewAccent: "#007acc",
                                    previewText: "#ffffff",
                                    selected: false
                                },
                                {
                                    id: "light",
                                    label: qsTrId("aegra.settings.theme.light"),
                                    previewBg: "#eeeeee",
                                    previewCard: "#ffffff",
                                    previewAccent: "#0078d4",
                                    previewText: "#1e1e1e",
                                    selected: false
                                }
                            ]
                            delegate: Rectangle {
                                required property var modelData
                                width: 148
                                height: 112
                                radius: 6
                                color: Theme.colorInput
                                border.width: modelData.selected ? 2 : 1
                                border.color: modelData.selected
                                              ? Theme.colorAccentBlue : Theme.colorBorder
                                opacity: modelData.selected ? 1.0 : 0.55

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 6
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 52
                                        radius: 4
                                        color: modelData.previewBg
                                        border.width: 1
                                        border.color: Theme.colorBorder
                                        clip: true
                                        Row {
                                            anchors.fill: parent
                                            anchors.margins: 6
                                            spacing: 4
                                            Rectangle {
                                                width: 18
                                                height: parent.height
                                                radius: 2
                                                color: modelData.previewCard
                                            }
                                            Column {
                                                anchors.verticalCenter: parent.verticalCenter
                                                spacing: 3
                                                Rectangle {
                                                    width: 70
                                                    height: 8
                                                    radius: 2
                                                    color: modelData.previewAccent
                                                }
                                                Rectangle {
                                                    width: 54
                                                    height: 6
                                                    radius: 2
                                                    color: modelData.previewText
                                                    opacity: 0.55
                                                }
                                                Rectangle {
                                                    width: 40
                                                    height: 6
                                                    radius: 2
                                                    color: modelData.previewText
                                                    opacity: 0.35
                                                }
                                            }
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.label
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 12
                                        font.bold: modelData.selected
                                        font.family: Theme.fontFamily
                                        horizontalAlignment: Text.AlignHCenter
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: "\u2713"
                                        color: Theme.colorAccentBlue
                                        font.pixelSize: 12
                                        font.bold: true
                                        visible: modelData.selected
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Card {
                Layout.fillWidth: true
                Layout.preferredHeight: serviceColumn.implicitHeight + 70
                //% "Service"
                title: qsTrId("aegra.home.card.service")

                ColumnLayout {
                    id: serviceColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: 50
                    anchors.leftMargin: 20
                    anchors.rightMargin: 20
                    anchors.bottomMargin: 16
                    spacing: 8

                    Text {
                        //% "Connection"
                        text: qsTrId("aegra.settings.service.connection")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                    }
                    Text {
                        text: serviceClient.statusText
                              + (serviceClient.serviceVersion.length > 0
                                 ? (" · V" + serviceClient.serviceVersion) : "")
                        color: Theme.colorTextWhite
                        font.pixelSize: 14
                        font.bold: true
                        font.family: Theme.fontFamily
                    }
                    AppButton {
                        //% "Reconnect"
                        text: qsTrId("aegra.common.reconnect")
                        onClicked: serviceClient.reconnect()
                    }
                }
            }

            Item { Layout.preferredHeight: 20 }
        }
    }
}
