import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import ".."

// Add Repository form (Local + Network). Hosted in RepositoryPage right drawer.
// Local browse is drive-letter only (no folder drill-down).
Item {
    id: root

    signal finished()
    signal cancelled()

    readonly property var locationTypes: [
        {
            //% "Local"
            label: qsTrId("aegra.repository.type.local"),
            value: "local",
            icon: "\uD83D\uDCBB",
            color: "#4a9eff",
            pathHint: "e.g. \\\\192.168.1.1\\share",
            hasAuth: false,
            needsPath: false,
            needsConnect: false
        },
        {
            //% "Network"
            label: qsTrId("aegra.repository.type.network"),
            value: "network",
            icon: "\uD83C\uDF10",
            color: "#ff8c42",
            pathHint: "e.g. \\\\192.168.1.1\\share",
            hasAuth: true,
            needsPath: true,
            needsConnect: true
        }
    ]

    property int selectedTypeIndex: 0
    property bool isDefault: false
    property bool isConnecting: false
    property bool isConnected: false
    /// Selected local drive root (e.g. "D:\\") or network path after connect/select.
    property string selectedPath: ""
    property bool isSubmitting: false
    property bool pendingSetDefault: false
    property var driveList: []
    property bool isDriveListLoading: false
    /// Held across Add → import-offer → Import for the same name/locator.
    property string pendingName: ""
    property string pendingLocator: ""
    /// Left label column width so Name / Type rows stay aligned.
    readonly property int fieldLabelWidth: 72
    readonly property int fieldRowSpacing: 12
    readonly property int fieldControlHeight: 34

    function currentType() {
        return root.locationTypes[root.selectedTypeIndex]
    }

    function showError(message) {
        if (typeof serviceClient !== "undefined" && serviceClient)
            serviceClient.showToast(message, true)
    }

    function refreshBrowse() {
        if (typeof serviceClient === "undefined" || !serviceClient)
            return
        root.isDriveListLoading = true
        root.driveList = []
        if (currentType().value === "local") {
            root.driveList = serviceClient.listLocalRepositoryDrives() || []
            root.isDriveListLoading = false
            return
        }
        // Network validation and access belong to the asynchronous Service Add command.
        root.isDriveListLoading = false
    }

    function connectNetworkShare() {
        var pathVal = networkPathInput.text.trim()
        if (pathVal === "") {
            //% "Please enter a network path"
            root.showError(qsTrId("aegra.repository.please_enter_path"))
            return
        }
        if (!root.isValidNetworkRoot(pathVal)) {
            root.showError(qsTrId("aegra.repository.network_path_invalid"))
            return
        }
        if (typeof serviceClient === "undefined" || !serviceClient || !serviceClient.connected) {
            root.showError(qsTrId("aegra.repository.service_not_connected"))
            return
        }
        if (serviceClient.repositoryCommandBusy)
            return
        root.selectedPath = pathVal
        root.isConnected = false
        root.isConnecting = true
        serviceClient.connectRepositoryLocation(
                    pathVal, networkUserInput.text.trim(), networkPasswordInput.text,
                    networkDomainInput.text.trim())
    }

    function selectDrive(name) {
        root.selectedPath = name
        if (currentType().value === "network") {
            var base = networkPathInput.text.trim()
            if (base !== "" && name.indexOf("\\\\") !== 0 && name.indexOf(":") !== 1) {
                var last = base.charAt(base.length - 1)
                if (last === "\\" || last === "/")
                    root.selectedPath = base + name
                else
                    root.selectedPath = base + "\\" + name
            }
        }
    }

    function resetForm() {
        isConnecting = false
        isConnected = false
        isSubmitting = false
        pendingSetDefault = false
        selectedPath = ""
        isDefault = false
        isDriveListLoading = false
        driveList = []
        nameInput.text = ""
        networkPathInput.text = ""
        networkUserInput.text = ""
        networkPasswordInput.text = ""
        networkDomainInput.text = ""
        selectedTypeIndex = 0
        refreshBrowse()
    }

    function activate() {
        resetForm()
        nameInput.forceActiveFocus()
    }

    /// Append fixed repo subdirectory under the chosen root (e.g. D:\ → D:\AegraRepo).
    function repositoryLocatorFromRoot(rootPath) {
        var base = (rootPath || "").trim()
        if (base === "")
            return ""
        while (base.length > 1
               && (base.charAt(base.length - 1) === "\\" || base.charAt(base.length - 1) === "/"))
            base = base.substring(0, base.length - 1)
        // Keep drive root form "D:" so join becomes "D:\AegraRepo".
        if (base.length === 2 && base.charAt(1) === ":")
            return base + "\\AegraRepo"
        if (base.indexOf("\\\\") === 0)
            return base + "\\AegraRepo"
        return base + "\\AegraRepo"
    }

    function isValidNetworkRoot(path) {
        var value = (path || "").trim()
        if (value.indexOf("\\\\") !== 0)
            return false
        var serverEnd = value.indexOf("\\", 2)
        return serverEnd > 2 && serverEnd < value.length - 1
    }

    function submit() {
        var nameVal = nameInput.text.trim()
        if (nameVal === "") {
            //% "Please enter a name"
            root.showError(qsTrId("aegra.repository.please_enter_name"))
            return
        }
        var rootPath = ""
        if (currentType().value === "local") {
            rootPath = root.selectedPath.trim()
            if (rootPath === "") {
                //% "Please select a drive"
                root.showError(qsTrId("aegra.repository.please_select_drive"))
                return
            }
        } else {
            if (!root.isConnected) {
                root.showError(qsTrId("aegra.repository.please_connect_first"))
                return
            }
            rootPath = root.selectedPath.trim()
            if (rootPath === "")
                rootPath = networkPathInput.text.trim()
            if (rootPath === "") {
                //% "Please enter a network path"
                root.showError(qsTrId("aegra.repository.please_enter_path"))
                return
            }
            if (!root.isValidNetworkRoot(rootPath)) {
                root.showError(qsTrId("aegra.repository.network_path_invalid"))
                return
            }
        }
        var pathVal = root.repositoryLocatorFromRoot(rootPath)
        if (typeof serviceClient === "undefined" || !serviceClient || !serviceClient.connected) {
            //% "Service is not connected"
            root.showError(qsTrId("aegra.repository.service_not_connected"))
            return
        }
        if (serviceClient.repositoryCommandBusy)
            return

        // Repository is a storage locator only. Archive encryption is configured on the
        // Backup Schedule wizard (encryption_enabled + archive_password), not here.
        root.pendingName = nameVal
        root.pendingLocator = pathVal
        root.isSubmitting = true
        root.pendingSetDefault = root.isDefault
        if (currentType().value === "network") {
            serviceClient.addRepositoryConnection(
                        nameVal, pathVal,
                        networkUserInput.text.trim(),
                        networkPasswordInput.text,
                        networkDomainInput.text.trim())
        } else {
            serviceClient.addRepositoryConnection(nameVal, pathVal)
        }
    }

    function confirmImportExisting() {
        if (root.pendingName === "" || root.pendingLocator === "")
            return
        if (typeof serviceClient === "undefined" || !serviceClient || !serviceClient.connected)
            return
        if (serviceClient.repositoryCommandBusy)
            return
        root.isSubmitting = true
        if (currentType().value === "network") {
            serviceClient.importRepositoryConnection(
                        root.pendingName, root.pendingLocator,
                        networkUserInput.text.trim(),
                        networkPasswordInput.text,
                        networkDomainInput.text.trim())
        } else {
            serviceClient.importRepositoryConnection(root.pendingName, root.pendingLocator)
        }
    }

    function declineImportExisting() {
        root.pendingSetDefault = false
        root.pendingName = ""
        root.pendingLocator = ""
    }

    Connections {
        target: typeof serviceClient !== "undefined" ? serviceClient : null
        enabled: typeof serviceClient !== "undefined" && serviceClient !== null

        function onRepositoryCommandChanged() {
            if (root.isConnecting) {
                if (serviceClient.repositoryCommandBusy)
                    return
                root.isConnecting = false
                var connectError = serviceClient.repositoryCommandErrorText || ""
                if (connectError.length > 0) {
                    root.isConnected = false
                    root.showError(connectError)
                    return
                }
                root.isConnected = true
                return
            }
            if (!root.isSubmitting)
                return
            if (serviceClient.repositoryCommandBusy)
                return
            root.isSubmitting = false
            var code = serviceClient.repositoryCommandErrorCode || ""
            // Disk has a valid Repository not registered in the control plane — offer Import.
            if (code === "repository.import_available") {
                importOfferDialog.open()
                return
            }
            var err = serviceClient.repositoryCommandErrorText || ""
            if (err.length > 0) {
                root.showError(err)
                root.pendingSetDefault = false
                root.pendingName = ""
                root.pendingLocator = ""
                return
            }
            if (root.pendingSetDefault) {
                var id = serviceClient.selectedRepositoryConnectionId || ""
                if (id.length > 0)
                    serviceClient.setDefaultRepositoryConnection(id)
            }
            root.pendingSetDefault = false
            root.pendingName = ""
            root.pendingLocator = ""
            root.finished()
        }

        function onRepositoryDirectoriesChanged() {
            root.isDriveListLoading = serviceClient.repositoryDirectoriesLoading
            if (root.isDriveListLoading)
                return
            root.driveList = serviceClient.repositoryDirectories || []
            var browseError = serviceClient.repositoryDirectoriesErrorText || ""
            if (browseError.length > 0)
                root.showError(browseError)
        }
    }

    // Offer Import when Add hits a valid on-disk repository not yet registered.
    Popup {
        id: importOfferDialog
        parent: Overlay.overlay
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        anchors.centerIn: Overlay.overlay
        width: Math.min(420, Overlay.overlay ? Overlay.overlay.width - 48 : 420)
        padding: 20
        property bool committing: false

        onClosed: {
            if (!importOfferDialog.committing)
                root.declineImportExisting()
            importOfferDialog.committing = false
        }

        background: Rectangle {
            color: Theme.colorPopup
            radius: 16
            border.width: 1
            border.color: Theme.colorBorder
        }

        contentItem: ColumnLayout {
            spacing: 16

            Text {
                Layout.fillWidth: true
                //% "Import repository"
                text: qsTrId("aegra.repository.import_offer.title")
                color: Theme.colorTextWhite
                font.pixelSize: 16
                font.bold: true
                font.family: Theme.fontFamily
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                //% "A repository already exists at this location but is not registered. Import it?"
                text: qsTrId("aegra.repository.import_offer.message")
                color: Theme.colorTextGrey
                font.pixelSize: 13
                font.family: Theme.fontFamily
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Item { Layout.fillWidth: true }
                AppButton {
                    //% "Cancel"
                    text: qsTrId("aegra.common.cancel")
                    Layout.preferredHeight: 36
                    onClicked: {
                        importOfferDialog.committing = true
                        root.declineImportExisting()
                        importOfferDialog.close()
                    }
                }
                AppButton {
                    //% "Import"
                    text: qsTrId("aegra.common.import")
                    primary: true
                    Layout.preferredHeight: 36
                    onClicked: {
                        importOfferDialog.committing = true
                        root.confirmImportExisting()
                        importOfferDialog.close()
                    }
                }
            }
        }
    }

    // Shared single-line placeholder style for text fields.
    component LinePlaceholder: Text {
        color: Theme.colorTextDim
        font.pixelSize: 13
        font.family: Theme.fontFamily
        elide: Text.ElideRight
        wrapMode: Text.NoWrap
        maximumLineCount: 1
        verticalAlignment: Text.AlignVCenter
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.colorCard
        radius: 4
        border.width: 1
        border.color: Theme.colorBorder

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 16

                // ---- Left: form (label | control columns) ----
                ColumnLayout {
                    Layout.preferredWidth: parent.width * 2 / 3
                    Layout.fillHeight: true
                    spacing: 0

                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                        ColumnLayout {
                            width: Math.max(280, parent.width)
                            spacing: 14

                            // Name
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: root.fieldRowSpacing
                                Text {
                                    Layout.preferredWidth: root.fieldLabelWidth
                                    Layout.alignment: Qt.AlignVCenter
                                    //% "Name"
                                    text: qsTrId("aegra.repository.field.name")
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                    wrapMode: Text.NoWrap
                                    maximumLineCount: 1
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: root.fieldControlHeight
                                    Layout.maximumHeight: root.fieldControlHeight
                                    radius: Theme.radiusControl
                                    color: Theme.colorInput
                                    border.width: 1
                                    border.color: nameInput.activeFocus ? Theme.colorAccentBlue
                                                                        : Theme.colorBorder
                                    clip: true
                                    TextInput {
                                        id: nameInput
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 13
                                        font.family: Theme.fontFamily
                                        clip: true
                                        selectByMouse: true
                                        verticalAlignment: TextInput.AlignVCenter
                                        LinePlaceholder {
                                            anchors.fill: parent
                                            //% "e.g. My Backup Location"
                                            text: qsTrId("aegra.repository.name_placeholder")
                                            visible: nameInput.text === "" && !nameInput.activeFocus
                                        }
                                    }
                                }
                            }

                            // Type
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: root.fieldRowSpacing
                                Text {
                                    Layout.preferredWidth: root.fieldLabelWidth
                                    Layout.alignment: Qt.AlignVCenter
                                    //% "Type"
                                    text: qsTrId("aegra.repository.field.type")
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                    wrapMode: Text.NoWrap
                                    maximumLineCount: 1
                                }
                                Row {
                                    Layout.alignment: Qt.AlignVCenter
                                    spacing: 8
                                    Repeater {
                                        model: root.locationTypes
                                        delegate: Rectangle {
                                            required property int index
                                            required property var modelData
                                            width: Math.max(88, typeLabel.implicitWidth + 28)
                                            height: root.fieldControlHeight
                                            radius: Theme.radiusControl
                                            color: root.selectedTypeIndex === index
                                                   ? Theme.colorAccentBlue
                                                   : (typeBtnArea.containsMouse
                                                      ? Theme.colorButtonHover : Theme.colorButton)
                                            border.width: root.selectedTypeIndex === index ? 0 : 1
                                            border.color: Theme.colorBorder
                                            Text {
                                                id: typeLabel
                                                anchors.centerIn: parent
                                                text: modelData.icon + " " + modelData.label
                                                color: root.selectedTypeIndex === index
                                                       ? Theme.colorOnAccent : Theme.colorTextWhite
                                                font.pixelSize: 12
                                                font.family: Theme.fontFamily
                                                font.bold: root.selectedTypeIndex === index
                                            }
                                            MouseArea {
                                                id: typeBtnArea
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    root.selectedTypeIndex = index
                                                    root.isConnected = false
                                                    root.isConnecting = false
                                                    root.selectedPath = ""
                                                    root.driveList = []
                                                    if (root.locationTypes[index].value === "local")
                                                        root.refreshBrowse()
                                                }
                                            }
                                        }
                                    }
                                }
                                Item { Layout.fillWidth: true }
                            }

                            // Network path
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: root.fieldRowSpacing
                                visible: root.locationTypes[root.selectedTypeIndex].needsPath
                                Text {
                                    Layout.preferredWidth: root.fieldLabelWidth
                                    Layout.alignment: Qt.AlignVCenter
                                    //% "Network path"
                                    text: qsTrId("aegra.repository.field.network_path")
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                    wrapMode: Text.NoWrap
                                    maximumLineCount: 1
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: root.fieldControlHeight
                                    Layout.maximumHeight: root.fieldControlHeight
                                    radius: Theme.radiusControl
                                    color: Theme.colorInput
                                    border.width: 1
                                    border.color: networkPathInput.activeFocus
                                                  ? Theme.colorAccentBlue : Theme.colorBorder
                                    clip: true
                                    TextInput {
                                        id: networkPathInput
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 13
                                        font.family: Theme.fontFamily
                                        clip: true
                                        selectByMouse: true
                                        verticalAlignment: TextInput.AlignVCenter
                                        onTextChanged: {
                                            if (root.currentType().value === "network") {
                                                root.isConnected = false
                                                root.driveList = []
                                                if (root.selectedPath === "")
                                                    root.selectedPath = text.trim()
                                            }
                                        }
                                        LinePlaceholder {
                                            anchors.fill: parent
                                            text: root.locationTypes[root.selectedTypeIndex].pathHint
                                            visible: networkPathInput.text === ""
                                                     && !networkPathInput.activeFocus
                                        }
                                    }
                                }
                            }

                            // Network auth (aligned to same columns)
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 12
                                visible: root.locationTypes[root.selectedTypeIndex].hasAuth

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: root.fieldRowSpacing
                                    Text {
                                        Layout.preferredWidth: root.fieldLabelWidth
                                        Layout.alignment: Qt.AlignVCenter
                                        //% "Username"
                                        text: qsTrId("aegra.repository.field.username")
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 13
                                        font.family: Theme.fontFamily
                                        elide: Text.ElideRight
                                        wrapMode: Text.NoWrap
                                        maximumLineCount: 1
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: root.fieldControlHeight
                                        Layout.maximumHeight: root.fieldControlHeight
                                        radius: Theme.radiusControl
                                        color: Theme.colorInput
                                        border.width: 1
                                        border.color: Theme.colorBorder
                                        clip: true
                                        TextInput {
                                            id: networkUserInput
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 10
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 13
                                            font.family: Theme.fontFamily
                                            clip: true
                                            selectByMouse: true
                                            verticalAlignment: TextInput.AlignVCenter
                                            onTextChanged: {
                                                root.isConnected = false
                                                root.driveList = []
                                            }
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: root.fieldRowSpacing
                                    Text {
                                        Layout.preferredWidth: root.fieldLabelWidth
                                        Layout.alignment: Qt.AlignVCenter
                                        //% "Password"
                                        text: qsTrId("aegra.repository.field.password")
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 13
                                        font.family: Theme.fontFamily
                                        elide: Text.ElideRight
                                        wrapMode: Text.NoWrap
                                        maximumLineCount: 1
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: root.fieldControlHeight
                                        Layout.maximumHeight: root.fieldControlHeight
                                        radius: Theme.radiusControl
                                        color: Theme.colorInput
                                        border.width: 1
                                        border.color: Theme.colorBorder
                                        clip: true
                                        TextInput {
                                            id: networkPasswordInput
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 10
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 13
                                            font.family: Theme.fontFamily
                                            echoMode: TextInput.Password
                                            clip: true
                                            selectByMouse: true
                                            verticalAlignment: TextInput.AlignVCenter
                                            onTextChanged: {
                                                root.isConnected = false
                                                root.driveList = []
                                            }
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: root.fieldRowSpacing
                                    Text {
                                        Layout.preferredWidth: root.fieldLabelWidth
                                        Layout.alignment: Qt.AlignVCenter
                                        //% "Domain"
                                        text: qsTrId("aegra.repository.field.domain")
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 13
                                        font.family: Theme.fontFamily
                                        elide: Text.ElideRight
                                        wrapMode: Text.NoWrap
                                        maximumLineCount: 1
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: root.fieldControlHeight
                                        Layout.maximumHeight: root.fieldControlHeight
                                        radius: Theme.radiusControl
                                        color: Theme.colorInput
                                        border.width: 1
                                        border.color: Theme.colorBorder
                                        clip: true
                                        TextInput {
                                            id: networkDomainInput
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 10
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 13
                                            font.family: Theme.fontFamily
                                            clip: true
                                            selectByMouse: true
                                            verticalAlignment: TextInput.AlignVCenter
                                            onTextChanged: {
                                                root.isConnected = false
                                                root.driveList = []
                                            }
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: root.fieldRowSpacing
                                    Item { Layout.preferredWidth: root.fieldLabelWidth }
                                    Rectangle {
                                        Layout.preferredWidth: 110
                                        Layout.preferredHeight: root.fieldControlHeight
                                        radius: Theme.radiusControl
                                        visible: root.locationTypes[root.selectedTypeIndex]
                                                 .needsConnect
                                        color: {
                                            if (root.isConnecting)
                                                return Theme.colorButtonDisabled
                                            if (root.isConnected)
                                                return Theme.colorGreen
                                            return connectBtnArea.containsMouse
                                                   ? Theme.colorButtonHover : Theme.colorButton
                                        }
                                        Text {
                                            anchors.centerIn: parent
                                            text: {
                                                if (root.isConnecting)
                                                    //% "Connecting..."
                                                    return qsTrId("aegra.repository.connecting")
                                                if (root.isConnected)
                                                    //% "Connected"
                                                    return qsTrId("aegra.repository.connected_btn")
                                                //% "Connect"
                                                return qsTrId("aegra.repository.connect")
                                            }
                                            color: root.isConnected ? "white"
                                                                    : Theme.colorTextWhite
                                            font.pixelSize: 12
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                        }
                                        MouseArea {
                                            id: connectBtnArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            enabled: !root.isConnecting
                                            onClicked: root.connectNetworkShare()
                                        }
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                            }

                            Item { Layout.fillHeight: true }
                        }
                    }
                }

                // ---- Right: browse (drive letters only for Local) ----
                ColumnLayout {
                    Layout.preferredWidth: parent.width / 3
                    Layout.fillHeight: true
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Rectangle {
                            width: 3
                            height: 18
                            color: Theme.colorAccentBlue
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Text {
                            //% "Browse"
                            text: qsTrId("aegra.repository.browse")
                            color: Theme.colorTextWhite
                            font.pixelSize: 14
                            font.bold: true
                            font.family: Theme.fontFamily
                            Layout.alignment: Qt.AlignVCenter
                            Layout.fillWidth: true
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: {
                            if (currentType().value === "local")
                                //% "Please select a drive"
                                return qsTrId("aegra.repository.please_select_drive")
                            if (!root.isConnected)
                                //% "Connect first"
                                return qsTrId("aegra.repository.please_connect_first")
                            //% "Please select a drive or folder"
                            return qsTrId("aegra.repository.please_select_path")
                        }
                        color: Theme.colorTextGrey
                        font.pixelSize: 10
                        font.family: Theme.fontFamily
                        elide: Text.ElideRight
                        wrapMode: Text.NoWrap
                        maximumLineCount: 1
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: Theme.colorBg
                        radius: 4
                        border.width: 1
                        border.color: Theme.colorBorder

                        Text {
                            anchors.centerIn: parent
                            visible: root.isDriveListLoading
                            //% "Loading"
                            text: qsTrId("aegra.common.loading")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                        }

                        Text {
                            anchors.centerIn: parent
                            width: parent.width - 24
                            visible: !root.isDriveListLoading && root.driveList.length === 0
                            text: {
                                var t = currentType()
                                if (t.needsConnect && !root.isConnected)
                                    //% "Connect first"
                                    return qsTrId("aegra.repository.please_connect_first")
                                if (t.value === "local")
                                    //% "Please select a drive"
                                    return qsTrId("aegra.repository.please_select_drive")
                                //% "No folders"
                                return qsTrId("aegra.repository.no_folders")
                            }
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                        }

                        ScrollView {
                            anchors.fill: parent
                            anchors.margins: 1
                            clip: true
                            contentWidth: availableWidth
                            visible: !root.isDriveListLoading && root.driveList.length > 0

                            Column {
                                width: parent.width
                                spacing: 1
                                padding: 4

                                Repeater {
                                    model: root.driveList
                                    delegate: Rectangle {
                                        required property var modelData
                                        width: parent.width - 4
                                        height: 34
                                        radius: 3
                                        color: {
                                            if (root.isDriveSelected(modelData))
                                                return Qt.rgba(Theme.colorAccentBlue.r,
                                                               Theme.colorAccentBlue.g,
                                                               Theme.colorAccentBlue.b, 0.35)
                                            return driveItemHover.containsMouse
                                                   ? Theme.colorHover : "transparent"
                                        }
                                        anchors.horizontalCenter: parent.horizontalCenter

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            spacing: 8
                                            Rectangle {
                                                width: 16
                                                height: 16
                                                radius: 3
                                                color: root.isDriveSelected(modelData)
                                                       ? Theme.colorAccentBlue : "transparent"
                                                border.width: 2
                                                border.color: root.isDriveSelected(modelData)
                                                              ? Theme.colorAccentBlue
                                                              : Theme.colorTextGrey
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: "\u2713"
                                                    color: "white"
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                    visible: root.isDriveSelected(modelData)
                                                }
                                            }
                                            Text {
                                                text: {
                                                    var t = root.locationTypes[root.selectedTypeIndex].value
                                                    var icon = t === "local"
                                                               ? "\uD83D\uDCBE " : "\uD83D\uDCC1 "
                                                    return icon + modelData
                                                }
                                                color: Theme.colorTextWhite
                                                font.pixelSize: 12
                                                font.family: Theme.fontFamily
                                                elide: Text.ElideRight
                                                wrapMode: Text.NoWrap
                                                maximumLineCount: 1
                                                Layout.fillWidth: true
                                            }
                                        }
                                        MouseArea {
                                            id: driveItemHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            // Single click only — no folder expand.
                                            onClicked: root.selectDrive(modelData)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: root.selectedPath !== "" ? 26 : 0
                        color: Qt.rgba(Theme.colorAccentBlue.r, Theme.colorAccentBlue.g,
                                       Theme.colorAccentBlue.b, 0.12)
                        radius: 4
                        visible: root.selectedPath !== ""
                        clip: true
                        Text {
                            anchors.fill: parent
                            anchors.margins: 8
                            //% "Selected: %1"
                            text: qsTrId("aegra.repository.selected_path")
                                  .arg(root.selectedPath)
                            color: Theme.colorAccentBlue
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideMiddle
                            wrapMode: Text.NoWrap
                            maximumLineCount: 1
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 10
                spacing: 8

                RowLayout {
                    spacing: 4
                    Rectangle {
                        width: 16
                        height: 16
                        radius: 3
                        color: root.isDefault ? Theme.colorAccentBlue : "transparent"
                        border.width: 1
                        border.color: root.isDefault ? Theme.colorAccentBlue : Theme.colorTextGrey
                        Text {
                            anchors.centerIn: parent
                            text: "\u2713"
                            color: "white"
                            font.pixelSize: 10
                            font.bold: true
                            visible: root.isDefault
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.isDefault = !root.isDefault
                        }
                    }
                    Text {
                        //% "Set as default"
                        text: qsTrId("aegra.repository.set_as_default")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.isDefault = !root.isDefault
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                AppButton {
                    //% "Cancel"
                    text: qsTrId("aegra.common.cancel")
                    enabled: !root.isSubmitting
                    onClicked: root.cancelled()
                }
                AppButton {
                    //% "Add"
                    text: qsTrId("aegra.common.add")
                    primary: true
                    enabled: !root.isSubmitting && !serviceClient.repositoryCommandBusy
                             && serviceClient.connected
                             && (root.currentType().value !== "network" || root.isConnected)
                    onClicked: root.submit()
                }
            }
        }
    }

    function isDriveSelected(name) {
        if (!name || root.selectedPath === "")
            return false
        if (root.selectedPath === name)
            return true
        // Network: selected path may be base + child folder name.
        if (currentType().value === "network") {
            var base = networkPathInput.text.trim()
            if (base === "")
                return false
            var last = base.charAt(base.length - 1)
            var joined = (last === "\\" || last === "/") ? (base + name) : (base + "\\" + name)
            return root.selectedPath === joined
        }
        return false
    }
}
