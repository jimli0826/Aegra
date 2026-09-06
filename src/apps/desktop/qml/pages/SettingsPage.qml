import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Item {
    id: root
    signal closeRequested()

    property int currentSection: 0
    readonly property int menuWidth: 220

    function valueIndex(model, value) {
        for (let i = 0; i < model.length; ++i) {
            if (model[i].value === value)
                return i
        }
        return 0
    }

    function cpuOptions() {
        const maximum = (typeof serviceClient !== "undefined" && serviceClient)
                        ? serviceClient.hostLogicalCpuCount : 1
        const result = []
        for (let value = 1; value <= maximum; ++value)
            result.push({ value: value, label: value.toString() })
        return result
    }

    function memoryOptions() {
        const budget = (typeof serviceClient !== "undefined" && serviceClient)
                       ? serviceClient.bootCheckMemoryBudgetMib : 4096
        const system = (typeof serviceClient !== "undefined" && serviceClient)
                       ? serviceClient.hostPhysicalMemoryMib : 4096
        const maximum = Math.max(2048, Math.min(32768, budget, system))
        const result = []
        for (let value = 2048; value <= maximum; value += 1024)
            result.push({ value: value, label: (value / 1024).toString() + " GB" })
        return result
    }

    function concurrencyOptions(memoryMib) {
        const budget = (typeof serviceClient !== "undefined" && serviceClient)
                       ? serviceClient.bootCheckMemoryBudgetMib : 8192
        const maximum = Math.max(1, Math.min(32, Math.floor(budget / memoryMib)))
        const result = []
        for (let value = 1; value <= maximum; ++value)
            result.push({ value: value, label: value.toString() })
        return result
    }

    function defaultHypervisorError() {
        if (typeof serviceClient === "undefined" || !serviceClient)
            return ""
        return serviceClient.defaultBootCheckHypervisor === 1
                ? serviceClient.virtualBoxUnavailableText
                : serviceClient.hyperVUnavailableText
    }

    //% "Settings"
    Accessible.name: qsTrId("aegra.nav.settings")

    component CategoryButton: Item {
        id: categoryButton

        property string label: ""
        property string iconName: ""
        property bool selected: false
        signal activated()

        readonly property bool hovered: categoryMouse.containsMouse

        Layout.fillWidth: true
        Layout.preferredHeight: 44

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusControl
            color: categoryButton.selected ? Theme.colorInput
                                               : (categoryButton.hovered ? Theme.colorMenuHoverBg
                                                                         : "transparent")
            border.width: categoryButton.selected ? 1 : 0
            border.color: Theme.colorBorder
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 12

            NavIcon {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                name: categoryButton.iconName
                color: categoryButton.selected ? Theme.colorAccentBlue : Theme.colorMenuIdle
            }
            Text {
                Layout.fillWidth: true
                text: categoryButton.label
                color: categoryButton.selected ? Theme.colorTextWhite : Theme.colorMenuIdle
                font.pixelSize: 14
                font.weight: categoryButton.selected ? Font.DemiBold : Font.Normal
                font.family: Theme.fontFamily
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
        }

        MouseArea {
            id: categoryMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: categoryButton.activated()
        }
    }

    component SettingCard: Rectangle {
        id: settingCard

        property string titleText: ""
        property string descriptionText: ""
        property string errorText: ""
        default property alias controlData: controlHost.data

        Layout.fillWidth: true
        Layout.preferredHeight: errorText.length > 0 ? 140 : 108
        radius: Theme.radiusCard
        color: Theme.colorCard
        border.width: 1
        border.color: Theme.colorBorder

        RowLayout {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 24

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Text {
                    Layout.fillWidth: true
                    text: settingCard.titleText
                    color: Theme.colorTextWhite
                    font.pixelSize: 14
                    font.bold: true
                    font.family: Theme.fontFamily
                }
                Text {
                    Layout.fillWidth: true
                    text: settingCard.descriptionText
                    color: Theme.colorTextGrey
                    font.pixelSize: 12
                    font.family: Theme.fontFamily
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: settingCard.errorText.length > 0
                    text: settingCard.errorText
                    color: Theme.colorAccentRed
                    font.pixelSize: 12
                    font.family: Theme.fontFamily
                    wrapMode: Text.WordWrap
                }
            }

            Item {
                id: controlHost
                Layout.preferredWidth: 220
                Layout.preferredHeight: 34
            }
        }
    }

    Item {
        id: categoryPane
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        width: root.menuWidth

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusWindow
            color: Theme.colorSidebar
        }

        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: Math.max(0, parent.width - Theme.radiusWindow)
            color: Theme.colorSidebar
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: 18
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 6

            Text {
                Layout.leftMargin: 10
                Layout.bottomMargin: 10
                //% "Settings"
                text: qsTrId("aegra.nav.settings")
                color: Theme.colorTextWhite
                font.pixelSize: 18
                font.bold: true
                font.family: Theme.fontFamily
            }

            CategoryButton {
                label: qsTrId("aegra.settings.category.general")
                iconName: "settings"
                selected: root.currentSection === 0
                onActivated: root.currentSection = 0
            }
            CategoryButton {
                label: qsTrId("aegra.settings.category.appearance")
                iconName: "appearance"
                selected: root.currentSection === 2
                onActivated: root.currentSection = 2
            }
            CategoryButton {
                label: qsTrId("aegra.settings.category.verify")
                iconName: "verify"
                selected: root.currentSection === 3
                onActivated: root.currentSection = 3
            }
            CategoryButton {
                label: qsTrId("aegra.settings.category.boot_check")
                iconName: "boot_check"
                selected: root.currentSection === 1
                onActivated: root.currentSection = 1
            }

            Item { Layout.fillHeight: true }
        }
    }

    Rectangle {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: categoryPane.right
        width: 1
        color: Theme.colorSidebarDivider
    }

    Button {
        id: closeBtn
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 6
        anchors.rightMargin: 8
        width: 32
        height: 28
        z: 10
        text: "\u2715"
        Accessible.name: qsTrId("aegra.common.close")
        background: Rectangle {
            color: closeBtn.hovered ? Theme.colorHover : "transparent"
            radius: 4
        }
        contentItem: Text {
            text: closeBtn.text
            color: Theme.colorTextWhite
            font.pixelSize: 14
            font.family: Theme.fontFamily
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        onClicked: root.closeRequested()
    }

    StackLayout {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: categoryPane.right
        anchors.right: parent.right
        anchors.topMargin: 18
        anchors.leftMargin: 28
        anchors.rightMargin: 28
        currentIndex: root.currentSection

        Item {
            ColumnLayout {
                anchors.fill: parent
                spacing: 16

                PageHeader {
                    Layout.fillWidth: true
                    Layout.rightMargin: 32
                    title: qsTrId("aegra.settings.category.general")
                }

                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: width
                    contentHeight: generalColumn.implicitHeight + 16
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    ColumnLayout {
                        id: generalColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 12

                        SettingCard {
                            titleText: qsTrId("aegra.settings.language")
                            descriptionText: qsTrId("aegra.settings.language_desc")

                            SettingsComboBox {
                                id: languageCombo
                                anchors.fill: parent
                                model: localeController.availableLanguages
                                currentIndex: {
                                    const languages = localeController.availableLanguages
                                    for (let i = 0; i < languages.length; ++i) {
                                        if (languages[i].tag === localeController.language)
                                            return i
                                    }
                                    return 0
                                }
                                onActivated: function(index) {
                                    const languages = localeController.availableLanguages
                                    if (index >= 0 && index < languages.length)
                                        localeController.setLanguage(languages[index].tag)
                                }
                            }
                        }

                        SettingCard {
                            titleText: qsTrId("aegra.settings.close_action")
                            descriptionText: qsTrId("aegra.settings.close_action_desc")

                            SettingsComboBox {
                                id: closeActionCombo
                                anchors.fill: parent
                                model: [
                                    { id: "hide", label: qsTrId("aegra.settings.close_action.hide") },
                                    { id: "quit", label: qsTrId("aegra.settings.close_action.quit") }
                                ]
                                currentIndex: desktopShell.closeAction === "quit" ? 1 : 0
                                onActivated: function(index) {
                                    const item = model[index]
                                    if (item)
                                        desktopShell.setCloseAction(item.id)
                                }
                            }
                        }

                        SettingCard {
                            titleText: qsTrId("aegra.settings.job_retention")
                            descriptionText: qsTrId("aegra.settings.job_retention_desc")
                            errorText: (typeof serviceClient !== "undefined" && serviceClient)
                                       ? serviceClient.serviceSettingsErrorText : ""
                            opacity: retentionCombo.enabled ? 1.0 : 0.65

                            SettingsComboBox {
                                id: retentionCombo
                                anchors.fill: parent
                                enabled: typeof serviceClient !== "undefined" && serviceClient
                                         && serviceClient.serviceSettingsAvailable
                                         && !serviceClient.serviceSettingsLoading
                                         && !serviceClient.serviceSettingsBusy
                                model: [
                                    { months: 1, label: qsTrId("aegra.settings.job_retention.1_month") },
                                    { months: 3, label: qsTrId("aegra.settings.job_retention.3_months") },
                                    { months: 6, label: qsTrId("aegra.settings.job_retention.6_months") }
                                ]
                                currentIndex: {
                                    if (typeof serviceClient === "undefined" || !serviceClient)
                                        return 1
                                    const months = serviceClient.jobRetentionMonths
                                    return months === 1 ? 0 : (months === 6 ? 2 : 1)
                                }
                                onActivated: function(index) {
                                    if (typeof serviceClient === "undefined" || !serviceClient)
                                        return
                                    const item = model[index]
                                    if (!item)
                                        return
                                    if (!serviceClient.setJobRetentionMonths(item.months)) {
                                        const months = serviceClient.jobRetentionMonths
                                        currentIndex = months === 1 ? 0 : (months === 6 ? 2 : 1)
                                    }
                                }
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 4
                        }
                    }
                }
            }
        }

        Item {
            ColumnLayout {
                anchors.fill: parent
                spacing: 16

                PageHeader {
                    Layout.fillWidth: true
                    Layout.rightMargin: 32
                    title: qsTrId("aegra.settings.category.boot_check")
                }

                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: width
                    contentHeight: bootCheckColumn.implicitHeight + 16
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    ColumnLayout {
                        id: bootCheckColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 12

                        SettingCard {
                            titleText: qsTrId("aegra.settings.boot_check.default_hypervisor")
                            descriptionText: qsTrId(
                                                 "aegra.settings.boot_check.default_hypervisor_desc")
                            errorText: serviceClient && serviceClient.serviceSettingsErrorText.length
                                       ? serviceClient.serviceSettingsErrorText
                                       : root.defaultHypervisorError()

                            RowLayout {
                                anchors.fill: parent
                                spacing: 6

                                SettingsComboBox {
                                    id: defaultHypervisorCombo
                                    Layout.fillWidth: true
                                    enabled: serviceClient && serviceClient.serviceSettingsAvailable
                                             && !serviceClient.serviceSettingsLoading
                                             && !serviceClient.serviceSettingsBusy
                                    model: [
                                        { value: 1, label: "VirtualBox",
                                          iconSource: "qrc:/Aegra/icons/virtualbox.png" },
                                        { value: 2, label: "Hyper-V",
                                          iconSource: "qrc:/Aegra/icons/hyperv.png" }
                                    ]
                                    currentIndex: root.valueIndex(model, serviceClient
                                        ? serviceClient.defaultBootCheckHypervisor : 1)
                                    onActivated: function(index) {
                                        const item = model[index]
                                        if (item && serviceClient)
                                            serviceClient.setBootCheckSettings(item.value,
                                                serviceClient.bootCheckCpuCount,
                                                serviceClient.bootCheckMemoryMib,
                                                serviceClient.bootCheckConcurrency)
                                    }
                                }

                                MouseArea {
                                    id: settingsHypervisorRefreshArea
                                    Layout.preferredWidth: 34
                                    Layout.preferredHeight: 34
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    enabled: serviceClient && !serviceClient.hypervisorProbing
                                    onClicked: serviceClient.refreshHypervisorStatus()

                                    Rectangle {
                                        anchors.fill: parent
                                        radius: Theme.radiusControl
                                        color: settingsHypervisorRefreshArea.pressed
                                               ? Theme.colorButtonHover
                                               : (settingsHypervisorRefreshArea.containsMouse
                                                  ? Theme.colorHover : "transparent")

                                        NavIcon {
                                            id: settingsHypervisorRefreshIcon
                                            anchors.centerIn: parent
                                            width: 17
                                            height: 17
                                            name: "refresh"
                                            color: settingsHypervisorRefreshArea.containsMouse
                                                   ? Theme.colorAccentBlue : Theme.colorTextGrey

                                            RotationAnimation on rotation {
                                                running: serviceClient
                                                         && serviceClient.hypervisorProbing
                                                from: 0
                                                to: 360
                                                duration: 1000
                                                loops: Animation.Infinite
                                                onRunningChanged: {
                                                    if (!running)
                                                        settingsHypervisorRefreshIcon.rotation = 0
                                                }
                                            }
                                        }
                                    }

                                    ToolTip.visible: containsMouse
                                    ToolTip.delay: 400
                                    ToolTip.text: qsTrId(
                                                      "aegra.backup.post.hypervisor_refresh")
                                }
                            }
                        }

                        SettingCard {
                            titleText: qsTrId("aegra.settings.boot_check.cpu")
                            descriptionText: qsTrId("aegra.settings.boot_check.cpu_desc")
                            errorText: serviceClient ? serviceClient.serviceSettingsErrorText : ""

                            SettingsComboBox {
                                id: bootCpuCombo
                                anchors.fill: parent
                                enabled: serviceClient && serviceClient.serviceSettingsAvailable
                                         && !serviceClient.serviceSettingsLoading
                                         && !serviceClient.serviceSettingsBusy
                                model: root.cpuOptions()
                                currentIndex: root.valueIndex(model, serviceClient
                                                             ? serviceClient.bootCheckCpuCount : 1)
                                onActivated: function(index) {
                                    const item = model[index]
                                    if (item && serviceClient)
                                        serviceClient.setBootCheckSettings(
                                            serviceClient.defaultBootCheckHypervisor, item.value,
                                            serviceClient.bootCheckMemoryMib,
                                            serviceClient.bootCheckConcurrency)
                                }
                            }
                        }

                        SettingCard {
                            titleText: qsTrId("aegra.settings.boot_check.memory")
                            descriptionText: qsTrId("aegra.settings.boot_check.memory_desc").arg(
                                                 serviceClient
                                                 ? Math.floor(serviceClient.hostPhysicalMemoryMib / 1024)
                                                 : 0)

                            SettingsComboBox {
                                id: bootMemoryCombo
                                anchors.fill: parent
                                enabled: bootCpuCombo.enabled
                                model: root.memoryOptions()
                                currentIndex: root.valueIndex(model, serviceClient
                                                             ? serviceClient.bootCheckMemoryMib : 4096)
                                onActivated: function(index) {
                                    const item = model[index]
                                    if (!item || !serviceClient)
                                        return
                                    const maximum = Math.max(1, Math.min(32, Math.floor(
                                        serviceClient.bootCheckMemoryBudgetMib / item.value)))
                                    serviceClient.setBootCheckSettings(
                                        serviceClient.defaultBootCheckHypervisor,
                                        serviceClient.bootCheckCpuCount, item.value,
                                        Math.min(serviceClient.bootCheckConcurrency, maximum))
                                }
                            }
                        }

                        SettingCard {
                            titleText: qsTrId("aegra.settings.boot_check.concurrency")
                            descriptionText: qsTrId("aegra.settings.boot_check.concurrency_desc").arg(
                                                 serviceClient
                                                 ? serviceClient.bootCheckEffectiveConcurrency : 0)

                            SettingsComboBox {
                                id: bootConcurrencyCombo
                                anchors.fill: parent
                                enabled: bootCpuCombo.enabled
                                model: root.concurrencyOptions(serviceClient
                                                               ? serviceClient.bootCheckMemoryMib : 4096)
                                currentIndex: root.valueIndex(model, serviceClient
                                                             ? serviceClient.bootCheckConcurrency : 1)
                                onActivated: function(index) {
                                    const item = model[index]
                                    if (item && serviceClient)
                                        serviceClient.setBootCheckSettings(
                                            serviceClient.defaultBootCheckHypervisor,
                                            serviceClient.bootCheckCpuCount,
                                            serviceClient.bootCheckMemoryMib, item.value)
                                }
                            }
                        }

                        Item { Layout.fillWidth: true; Layout.preferredHeight: 4 }
                    }
                }
            }
        }

        Item {
            ColumnLayout {
                anchors.fill: parent
                spacing: 16

                PageHeader {
                    Layout.fillWidth: true
                    Layout.rightMargin: 32
                    title: qsTrId("aegra.settings.category.appearance")
                }

                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: width
                    contentHeight: appearanceColumn.implicitHeight + 16
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    ColumnLayout {
                        id: appearanceColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 12

                        SettingCard {
                            titleText: qsTrId("aegra.settings.theme")
                            descriptionText: qsTrId("aegra.settings.theme_desc")

                            SettingsComboBox {
                                id: themeCombo
                                anchors.fill: parent
                                model: Theme.themes
                                useThemeLabels: true
                                currentIndex: {
                                    const themes = Theme.themes
                                    for (let i = 0; i < themes.length; ++i) {
                                        if (themes[i].id === Theme.themeId)
                                            return i
                                    }
                                    return 0
                                }
                                onActivated: function(index) {
                                    const themes = Theme.themes
                                    if (index >= 0 && index < themes.length)
                                        Theme.setTheme(themes[index].id)
                                }
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 4
                        }
                    }
                }
            }
        }

        Item {
            ColumnLayout {
                anchors.fill: parent
                spacing: 16

                PageHeader {
                    Layout.fillWidth: true
                    Layout.rightMargin: 32
                    title: qsTrId("aegra.settings.category.verify")
                }

                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: width
                    contentHeight: verifyColumn.implicitHeight + 16
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    ColumnLayout {
                        id: verifyColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 12

                        SettingCard {
                            titleText: qsTrId("aegra.settings.verify.scope")
                            descriptionText: qsTrId("aegra.settings.verify.scope_desc")
                            errorText: serviceClient ? serviceClient.serviceSettingsErrorText : ""

                            SettingsComboBox {
                                id: verifyScopeCombo
                                anchors.fill: parent
                                enabled: serviceClient && serviceClient.serviceSettingsAvailable
                                         && !serviceClient.serviceSettingsLoading
                                         && !serviceClient.serviceSettingsBusy
                                model: [
                                    { value: 1, label: qsTrId("aegra.settings.verify.scope.single") },
                                    { value: 2, label: qsTrId("aegra.settings.verify.scope.full_chain") }
                                ]
                                currentIndex: serviceClient && serviceClient.verifyScope === 2 ? 1 : 0
                                onActivated: function(index) {
                                    const item = model[index]
                                    if (item && serviceClient)
                                        serviceClient.setVerifySettings(item.value,
                                                                        serviceClient.verifyConcurrency)
                                }
                            }
                        }

                        SettingCard {
                            titleText: qsTrId("aegra.settings.verify.concurrency")
                            descriptionText: qsTrId("aegra.settings.verify.concurrency_desc")

                            SettingsComboBox {
                                id: verifyConcurrencyCombo
                                anchors.fill: parent
                                enabled: verifyScopeCombo.enabled
                                model: {
                                    const result = []
                                    for (let value = 1; value <= 32; ++value)
                                        result.push({ value: value, label: value.toString() })
                                    return result
                                }
                                currentIndex: root.valueIndex(model, serviceClient
                                                             ? serviceClient.verifyConcurrency : 2)
                                onActivated: function(index) {
                                    const item = model[index]
                                    if (item && serviceClient)
                                        serviceClient.setVerifySettings(serviceClient.verifyScope,
                                                                        item.value)
                                }
                            }
                        }

                        Item { Layout.fillWidth: true; Layout.preferredHeight: 4 }
                    }
                }
            }
        }
    }

    Connections {
        target: typeof serviceClient !== "undefined" ? serviceClient : null
        function onServiceSettingsChanged() {
            if (typeof serviceClient === "undefined" || !serviceClient)
                return
            const months = serviceClient.jobRetentionMonths
            retentionCombo.currentIndex = months === 1 ? 0 : (months === 6 ? 2 : 1)
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
