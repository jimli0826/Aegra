import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Item {
    id: root
    signal closeRequested()
    //% "Settings"
    Accessible.name: qsTrId("aegra.nav.settings")

    // Close button — top right
    Button {
        id: closeBtn
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 10
        anchors.rightMargin: 10
        width: 32
        height: 28
        z: 10
        text: "\u2715"
        background: Rectangle {
            color: parent.hovered ? Theme.colorHover : "transparent"
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

    Flickable {
        anchors.top: closeBtn.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.topMargin: 4
        contentWidth: width
        contentHeight: mainCol.implicitHeight + 24
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: mainCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            anchors.topMargin: 4
            spacing: 16

            // ── Language card ─────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                radius: 16
                color: Theme.colorCard
                border.width: 0
                implicitHeight: langInner.implicitHeight + 40

                ColumnLayout {
                    id: langInner
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 20
                    spacing: 10

                    Text {
                        //% "Language"
                        text: qsTrId("aegra.settings.language")
                        color: Theme.colorTextWhite
                        font.pixelSize: 15
                        font.bold: true
                        font.family: Theme.fontFamily
                    }
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
                            radius: 6
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
                                radius: 6
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

            // ── Theme card ────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                radius: 16
                color: Theme.colorCard
                border.width: 0
                implicitHeight: themeInner.implicitHeight + 40

                ColumnLayout {
                    id: themeInner
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 20
                    spacing: 10

                    Text {
                        //% "Theme"
                        text: qsTrId("aegra.settings.theme")
                        color: Theme.colorTextWhite
                        font.pixelSize: 15
                        font.bold: true
                        font.family: Theme.fontFamily
                    }
                    Text {
                        Layout.fillWidth: true
                        //% "Choose a color theme for the desktop client"
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
                            model: Theme.themes
                            delegate: Rectangle {
                                id: themeCard
                                required property var modelData
                                readonly property bool selected: Theme.themeId === modelData.id
                                width: 140
                                height: 106
                                radius: 10
                                color: Theme.colorInput
                                border.width: selected ? 2 : 1
                                border.color: selected ? Theme.colorAccentBlue : Theme.colorBorder
                                Behavior on border.color { ColorAnimation { duration: 150 } }

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 6
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 50
                                        radius: 6
                                        color: modelData.previewBg
                                        clip: true
                                        Row {
                                            anchors.fill: parent
                                            anchors.margins: 6
                                            spacing: 4
                                            Rectangle { width: 16; height: parent.height; radius: 2; color: modelData.previewCard }
                                            Column {
                                                anchors.verticalCenter: parent.verticalCenter
                                                spacing: 3
                                                Rectangle { width: 64; height: 7; radius: 2; color: modelData.previewAccent }
                                                Rectangle { width: 50; height: 5; radius: 2; color: modelData.previewText; opacity: 0.55 }
                                                Rectangle { width: 36; height: 5; radius: 2; color: modelData.previewText; opacity: 0.35 }
                                            }
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: Theme.themeLabel(modelData)
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 11
                                        font.bold: themeCard.selected
                                        font.family: Theme.fontFamily
                                        horizontalAlignment: Text.AlignHCenter
                                        elide: Text.ElideRight
                                    }
                                    Item {
                                        Layout.alignment: Qt.AlignHCenter
                                        Layout.preferredWidth: 16
                                        Layout.preferredHeight: 14
                                        Text {
                                            anchors.centerIn: parent
                                            text: "\u2713"
                                            color: Theme.colorAccentBlue
                                            font.pixelSize: 12
                                            font.bold: true
                                            visible: themeCard.selected
                                        }
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Theme.setTheme(modelData.id)
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: themeCard.radius
                                        color: Theme.colorHover
                                        opacity: parent.containsMouse && !themeCard.selected ? 0.3 : 0
                                        Behavior on opacity { NumberAnimation { duration: 120 } }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ── Service card ──────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                radius: 16
                color: Theme.colorCard
                border.width: 0
                implicitHeight: serviceInner.implicitHeight + 40

                ColumnLayout {
                    id: serviceInner
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 20
                    spacing: 8

                    Text {
                        //% "Service"
                        text: qsTrId("aegra.home.card.service")
                        color: Theme.colorTextWhite
                        font.pixelSize: 15
                        font.bold: true
                        font.family: Theme.fontFamily
                    }
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

            Item { Layout.preferredHeight: 8 }
        }
    }
}
