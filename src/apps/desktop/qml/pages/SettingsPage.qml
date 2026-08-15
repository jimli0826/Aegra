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
                        Layout.preferredHeight: 34
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
                            radius: 8
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                        indicator: ComboBoxIndicator { combo: languageCombo }
                        contentItem: Text {
                            leftPadding: 12
                            rightPadding: 24
                            text: languageCombo.displayText
                            color: Theme.colorTextWhite
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        popup: Popup {
                            y: languageCombo.height + 2
                            width: languageCombo.width
                            padding: 4
                            implicitHeight: Math.min(200, contentItem.implicitHeight + 8)
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
                                radius: 8
                            }
                        }
                        delegate: ItemDelegate {
                            id: langItemDel
                            width: languageCombo.width - 8
                            height: 32
                            hoverEnabled: true
                            highlighted: languageCombo.highlightedIndex === index
                            contentItem: Text {
                                leftPadding: 10
                                text: modelData.label
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 6
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
                    ComboBox {
                        id: themeCombo
                        Layout.preferredWidth: 280
                        Layout.preferredHeight: 34
                        model: Theme.themes
                        currentIndex: {
                            const list = Theme.themes
                            for (let i = 0; i < list.length; ++i) {
                                if (list[i].id === Theme.themeId)
                                    return i
                            }
                            return 0
                        }
                        onActivated: function(index) {
                            const list = Theme.themes
                            if (index >= 0 && index < list.length)
                                Theme.setTheme(list[index].id)
                        }
                        background: Rectangle {
                            color: Theme.colorInput
                            radius: 8
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                        indicator: ComboBoxIndicator { combo: themeCombo }
                        contentItem: Text {
                            leftPadding: 12
                            rightPadding: 24
                            text: Theme.themeLabel(Theme.themes[themeCombo.currentIndex])
                            color: Theme.colorTextWhite
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        popup: Popup {
                            y: themeCombo.height + 2
                            width: themeCombo.width
                            padding: 4
                            implicitHeight: Math.min(200, contentItem.implicitHeight + 8)
                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                model: themeCombo.popup.visible ? themeCombo.delegateModel : null
                                currentIndex: themeCombo.highlightedIndex
                            }
                            background: Rectangle {
                                color: Theme.colorPopup
                                border.color: Theme.colorBorder
                                radius: 8
                            }
                        }
                        delegate: ItemDelegate {
                            id: themeItemDel
                            width: themeCombo.width - 8
                            height: 32
                            hoverEnabled: true
                            highlighted: themeCombo.highlightedIndex === index
                            contentItem: Text {
                                leftPadding: 10
                                text: Theme.themeLabel(modelData)
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                            background: Rectangle {
                                radius: 6
                                color: themeItemDel.highlighted
                                       ? Theme.colorHover
                                       : (themeItemDel.hovered ? Theme.colorHover : "transparent")
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
                        Layout.preferredHeight: 34
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
                            radius: 8
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                        indicator: ComboBoxIndicator { combo: retentionCombo }
                        contentItem: Text {
                            leftPadding: 12
                            rightPadding: 24
                            text: retentionCombo.displayText
                            color: Theme.colorTextWhite
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        popup: Popup {
                            y: retentionCombo.height + 2
                            width: retentionCombo.width
                            padding: 4
                            implicitHeight: Math.min(200, contentItem.implicitHeight + 8)
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
                                radius: 8
                            }
                        }
                        delegate: ItemDelegate {
                            id: retItemDel
                            width: retentionCombo.width - 8
                            height: 32
                            hoverEnabled: true
                            highlighted: retentionCombo.highlightedIndex === index
                            contentItem: Text {
                                leftPadding: 10
                                text: modelData.label
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 6
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
