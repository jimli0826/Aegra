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
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0; color: Theme.colorCard }
                    GradientStop { position: 1.0; color: Theme.colorCardEnd }
                }
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
                            id: langItemDel
                            width: languageCombo.width
                            height: 32
                            hoverEnabled: true
                            highlighted: languageCombo.highlightedIndex === index
                            contentItem: Text {
                                text: modelData.label
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 4
                                color: (langItemDel.hovered || langItemDel.highlighted) ? Theme.colorHover : "transparent"
                            }
                        }
                    }
                }
            }

            // ── Theme card ────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                radius: 16
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0; color: Theme.colorCard }
                    GradientStop { position: 1.0; color: Theme.colorCardEnd }
                }
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

            // ── Job retention card ────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                radius: 16
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0; color: Theme.colorCard }
                    GradientStop { position: 1.0; color: Theme.colorCardEnd }
                }
                border.width: 0
                implicitHeight: retentionInner.implicitHeight + 40
                enabled: typeof serviceClient !== "undefined" && serviceClient
                         && serviceClient.serviceSettingsAvailable

                ColumnLayout {
                    id: retentionInner
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 20
                    spacing: 10

                    Text {
                        //% "Job history retention"
                        text: qsTrId("aegra.settings.job_retention")
                        color: Theme.colorTextWhite
                        font.pixelSize: 15
                        font.bold: true
                        font.family: Theme.fontFamily
                    }
                    Text {
                        Layout.fillWidth: true
                        //% "Completed jobs older than this period are permanently deleted from the service."
                        text: qsTrId("aegra.settings.job_retention_desc")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                    }
                    ComboBox {
                        id: retentionCombo
                        Layout.preferredWidth: 280
                        Layout.preferredHeight: 36
                        enabled: typeof serviceClient !== "undefined" && serviceClient
                                 && serviceClient.serviceSettingsAvailable
                                 && !serviceClient.serviceSettingsLoading
                                 && !serviceClient.serviceSettingsBusy
                        model: [
                            { months: 1, label: qsTrId("aegra.settings.job_retention.1_month") },
                            { months: 3, label: qsTrId("aegra.settings.job_retention.3_months") },
                            { months: 6, label: qsTrId("aegra.settings.job_retention.6_months") }
                        ]
                        textRole: "label"
                        currentIndex: {
                            if (typeof serviceClient === "undefined" || !serviceClient)
                                return 1
                            const months = serviceClient.jobRetentionMonths
                            if (months === 1)
                                return 0
                            if (months === 6)
                                return 2
                            return 1
                        }
                        onActivated: function(index) {
                            if (typeof serviceClient === "undefined" || !serviceClient)
                                return
                            const item = model[index]
                            if (!item)
                                return
                            if (!serviceClient.setJobRetentionMonths(item.months)) {
                                // Revert combo if the request was not sent.
                                const months = serviceClient.jobRetentionMonths
                                if (months === 1)
                                    currentIndex = 0
                                else if (months === 6)
                                    currentIndex = 2
                                else
                                    currentIndex = 1
                            }
                        }
                        background: Rectangle {
                            color: Theme.colorInput
                            radius: 6
                            border.width: 1
                            border.color: retentionCombo.activeFocus || retentionCombo.popup.visible
                                          ? Theme.colorAccentBlue : Theme.colorBorder
                        }
                        contentItem: Text {
                            leftPadding: 12
                            rightPadding: 24
                            text: retentionCombo.displayText
                            color: Theme.colorTextWhite
                            font.pixelSize: 14
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        popup: Popup {
                            y: retentionCombo.height + 2
                            width: retentionCombo.width
                            implicitHeight: contentItem.implicitHeight
                            padding: 1
                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                model: retentionCombo.popup.visible
                                       ? retentionCombo.delegateModel : null
                                currentIndex: retentionCombo.highlightedIndex
                            }
                            background: Rectangle {
                                color: Theme.colorPopup
                                border.color: Theme.colorBorder
                                radius: 6
                            }
                        }
                        delegate: ItemDelegate {
                            id: retItemDel
                            width: retentionCombo.width
                            height: 32
                            hoverEnabled: true
                            highlighted: retentionCombo.highlightedIndex === index
                            contentItem: Text {
                                text: modelData.label
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 4
                                color: (retItemDel.hovered || retItemDel.highlighted) ? Theme.colorHover : "transparent"
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: typeof serviceClient !== "undefined" && serviceClient
                                 && serviceClient.serviceSettingsErrorText.length > 0
                        text: (typeof serviceClient !== "undefined" && serviceClient)
                              ? serviceClient.serviceSettingsErrorText : ""
                        color: Theme.colorAccentRed
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Item { Layout.preferredHeight: 8 }
        }
    }

    Connections {
        target: typeof serviceClient !== "undefined" ? serviceClient : null
        function onServiceSettingsChanged() {
            if (typeof serviceClient === "undefined" || !serviceClient)
                return
            const months = serviceClient.jobRetentionMonths
            if (months === 1)
                retentionCombo.currentIndex = 0
            else if (months === 6)
                retentionCombo.currentIndex = 2
            else
                retentionCombo.currentIndex = 1
        }
        function onStateChanged() {
            if (typeof serviceClient !== "undefined" && serviceClient
                    && serviceClient.serviceSettingsAvailable
                    && !serviceClient.serviceSettingsLoading
                    && !serviceClient.serviceSettingsBusy) {
                serviceClient.refreshServiceSettings()
            }
        }
    }

    Component.onCompleted: {
        if (typeof serviceClient !== "undefined" && serviceClient
                && serviceClient.serviceSettingsAvailable) {
            serviceClient.refreshServiceSettings()
        }
    }
}
