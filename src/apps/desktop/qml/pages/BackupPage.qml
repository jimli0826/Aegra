import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Visual baseline: backup/src/gui BackupPage — schedule list + Add Schedule Wizard step 1.
Item {
    id: root
    //% "Backup"
    Accessible.name: qsTrId("aegra.nav.backup")
    /// Request Main to switch to Home after a real backup job is accepted.
    signal navigateHomeRequested()

    property bool wizardOpen: false
    property int wizardStep: 1
    property int selectedLocationIndex: 0
    property var expandedDisks: ({})
    /// Volume selection keys "d{diskIndex}v{volIndex}" → true (old wizard multi-select).
    property var selectedVolumeKeys: ({})
    property int selectionEpoch: 0

    // Service-backed schedules (empty until list_schedules returns).
    readonly property var schedules: serviceClient.schedules || []

    readonly property var disksTree: {
        if (serviceClient.sources && serviceClient.sources.count > 0
                && serviceClient.sources.disksTree
                && serviceClient.sources.disksTree.length > 0)
            return serviceClient.sources.disksTree
        return []
    }

    // Destination list: only real repository connections (no demo locations).
    readonly property var locationModel: {
        if (serviceClient.connections && serviceClient.connections.count > 0)
            return null
        return []
    }

    function isDiskExpanded(index) {
        return expandedDisks[index] === true
    }

    function toggleDiskExpanded(index) {
        var next = Object.assign({}, expandedDisks)
        next[index] = !next[index]
        expandedDisks = next
    }

    function openWizard() {
        selectedLocationIndex = 0
        expandedDisks = ({})
        selectedVolumeKeys = ({})
        selectionEpoch = 0
        wizardStep = 1
        // Disks start collapsed (old wizard default: click chevron to expand).
        expandedDisks = ({})
        if (wizardStep2)
            wizardStep2.resetDefaults()
        wizardOpen = true
        if (serviceClient.connected) {
            serviceClient.refreshInventory()
            serviceClient.refreshConnections()
            serviceClient.refreshSchedules()
        }
    }

    function closeWizard() {
        wizardOpen = false
        wizardStep = 1
    }

    function volumeKey(diskIndex, volumeIndex) {
        return "d" + diskIndex + "v" + volumeIndex
    }

    function isVolumeSelected(diskIndex, volumeIndex) {
        var epoch = selectionEpoch
        return epoch >= 0 && selectedVolumeKeys[volumeKey(diskIndex, volumeIndex)] === true
    }

    function isVolumeSelectable(diskIndex, volumeIndex) {
        if (diskIndex < 0 || diskIndex >= disksTree.length)
            return false
        var vols = disksTree[diskIndex].volumes || []
        return volumeIndex >= 0 && volumeIndex < vols.length
                && vols[volumeIndex].selectable === true
    }

    function isDiskSelected(diskIndex) {
        var epoch = selectionEpoch
        if (epoch < 0 || diskIndex < 0 || diskIndex >= disksTree.length)
            return false
        var vols = disksTree[diskIndex].volumes || []
        var selectableCount = 0
        for (var i = 0; i < vols.length; ++i) {
            if (!isVolumeSelectable(diskIndex, i))
                continue
            selectableCount++
            if (!isVolumeSelected(diskIndex, i))
                return false
        }
        return selectableCount > 0
    }

    function toggleVolumeSelected(diskIndex, volumeIndex) {
        if (!isVolumeSelectable(diskIndex, volumeIndex))
            return
        var key = volumeKey(diskIndex, volumeIndex)
        var next = Object.assign({}, selectedVolumeKeys)
        if (next[key])
            delete next[key]
        else
            next[key] = true
        selectedVolumeKeys = next
        selectionEpoch++
    }

    function toggleDiskSelected(diskIndex) {
        if (diskIndex < 0 || diskIndex >= disksTree.length)
            return
        var vols = disksTree[diskIndex].volumes || []
        var allOn = isDiskSelected(diskIndex)
        var next = Object.assign({}, selectedVolumeKeys)
        for (var i = 0; i < vols.length; ++i) {
            if (!isVolumeSelectable(diskIndex, i))
                continue
            var key = volumeKey(diskIndex, i)
            if (allOn)
                delete next[key]
            else
                next[key] = true
        }
        selectedVolumeKeys = next
        selectionEpoch++
    }

    function selectedSourceName() {
        var names = []
        for (var d = 0; d < disksTree.length; ++d) {
            var vols = disksTree[d].volumes || []
            for (var v = 0; v < vols.length; ++v) {
                if (isVolumeSelectable(d, v) && isVolumeSelected(d, v)) {
                    var letter = vols[v].letter || ""
                    names.push(letter.length > 0 ? letter : (vols[v].name || ("vol" + v)))
                }
            }
        }
        if (names.length === 0)
            return "disk0"
        return names.join(",")
    }

    function selectedSources() {
        var sources = []
        for (var d = 0; d < disksTree.length; ++d) {
            var vols = disksTree[d].volumes || []
            for (var v = 0; v < vols.length; ++v) {
                if (!isVolumeSelectable(d, v) || !isVolumeSelected(d, v)
                        || !vols[v].sourceId)
                    continue
                var letter = vols[v].letter || ""
                sources.push({
                    "sourceId": vols[v].sourceId,
                    "displayName": letter.length > 0 ? letter : (vols[v].name || ("vol" + v))
                })
            }
        }
        return sources
    }

    /// Selected repository connection id for the wizard DESTINATION row, or empty.
    function selectedConnectionId() {
        var conns = serviceClient.connections
        if (!conns || conns.count <= 0)
            return ""
        var idx = selectedLocationIndex
        if (idx < 0 || idx >= conns.count)
            return ""
        if (typeof conns.isAvailableAt === "function" && !conns.isAvailableAt(idx))
            return ""
        if (typeof conns.connectionIdAt === "function") {
            var id = conns.connectionIdAt(idx)
            return id ? ("" + id) : ""
        }
        return ""
    }

    function canGoNext() {
        // Require at least one backup source and an available destination connection.
        // Empty Locations list (screenshot) must keep Next disabled.
        return selectedSources().length > 0 && selectedConnectionId().length > 0
    }

    function freqLabel(f) {
        var x = (f || "daily").toString().toLowerCase()
        if (x === "weekly")
            return qsTrId("aegra.backup.freq.weekly")
        if (x === "monthly")
            return qsTrId("aegra.backup.freq.monthly")
        return qsTrId("aegra.backup.freq.daily")
    }

    function timeOrNa(v) {
        var s = (v === undefined || v === null) ? "" : ("" + v).trim()
        return s.length > 0 ? s : qsTrId("aegra.common.not_available")
    }

    function createScheduleFromWizard() {
        var sources = selectedSources()
        var connId = selectedConnectionId()
        if (connId.length === 0 && serviceClient.defaultConnectionId)
            connId = serviceClient.defaultConnectionId() || ""
        if (sources.length === 0) {
            //% "Select at least one backup source"
            serviceClient.showToast(qsTrId("aegra.backup.schedule.missing_source"))
            return
        }
        if (!connId || connId.length === 0) {
            //% "Select a repository destination (Locations)"
            serviceClient.showToast(qsTrId("aegra.backup.schedule.missing_target"))
            return
        }
        var s2 = wizardStep2
        var frequency = s2 ? s2.frequency : "daily"
        var timeOfDay = s2 ? s2.timeOfDay : "02:00"
        var excludePage = s2 ? s2.excludePageHibernation : true
        var encryption = s2 ? s2.encryption : false
        var password = s2 ? (s2.password || "") : ""
        var passwordConfirm = s2 ? (s2.passwordConfirm || "") : ""
        if (encryption) {
            if (password.length === 0 || password.length > 32) {
                serviceClient.showToast(qsTrId("aegra.backup.opt.password_required"))
                return
            }
            if (password !== passwordConfirm) {
                serviceClient.showToast(qsTrId("aegra.backup.opt.password_mismatch"))
                return
            }
        }
        if (!serviceClient.createSchedule(sources, connId, frequency, timeOfDay, excludePage,
                                          encryption, encryption ? password : "")) {
            //% "Could not save schedule"
            serviceClient.showToast(qsTrId("aegra.backup.schedule.save_failed"))
            return
        }
        // First run uses wizard password; later Run uses schedule_id + wincred.
        serviceClient.startBackup(sources, connId, excludePage, encryption,
                                  encryption ? password : "")
        closeWizard()
    }

    function pad2(n) {
        return (n < 10 ? "0" : "") + n
    }

    function formatNowLocal() {
        var d = new Date()
        return d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate())
               + " " + pad2(d.getHours()) + ":" + pad2(d.getMinutes()) + ":" + pad2(d.getSeconds())
    }

    function formatNextRunLocal(timeOfDay) {
        var parts = (timeOfDay || "02:00").split(":")
        var h = parseInt(parts[0], 10)
        var m = parseInt(parts[1], 10)
        if (isNaN(h)) h = 2
        if (isNaN(m)) m = 0
        var d = new Date()
        d.setSeconds(0, 0)
        d.setHours(h, m, 0, 0)
        if (d.getTime() <= Date.now())
            d.setDate(d.getDate() + 1)
        return d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate())
               + " " + pad2(d.getHours()) + ":" + pad2(d.getMinutes())
    }

    property string pendingRunScheduleId: ""

    Connections {
        target: serviceClient
        function onBackupStartSucceeded(jobId) {
            root.pendingRunScheduleId = ""
            // Old product navigates to Home so the Tasks table shows the new job.
            root.navigateHomeRequested()
        }
        function onBackupStartFailed(message) {
            root.pendingRunScheduleId = ""
        }
        function onSchedulesChanged() {
            // Schedules reloaded from Service after create/toggle/delete.
        }
    }

    /// Run schedule now. backupType: 1 = full, 2 = incremental (service_protocol).
    function runSchedule(item, backupType) {
        if (!item || !item.enabled) {
            //% "Enable the schedule before running it"
            serviceClient.showToast(qsTrId("aegra.backup.run.disabled"))
            return
        }
        var type = (backupType === 2) ? 2 : 1
        if (serviceClient.connected && serviceClient.hasCapability("backup.start")) {
            var sourceIds = item.sourceIds || []
            var connectionId = item.connectionId || ""
            if (connectionId.length === 0)
                connectionId = serviceClient.defaultConnectionId()
            if (sourceIds.length > 0 && connectionId.length > 0) {
                root.pendingRunScheduleId = item.scheduleId || item.id || ""
                var excludePage = item.excludePageAndHibernation !== false
                var encryption = item.encryptionEnabled === true
                var scheduleId = item.scheduleId || item.id || ""
                if (serviceClient.startBackup(sourceIds, connectionId, excludePage, encryption,
                                              "", encryption ? scheduleId : "", type))
                    return
                root.pendingRunScheduleId = ""
                return
            }
            //% "No selectable source or repository connection for backup"
            serviceClient.showToast(qsTrId("aegra.backup.run.missing_target"))
            return
        }
        //% "Service not connected"
        serviceClient.showToast(qsTrId("aegra.backup.run.not_connected"))
    }

    function toggleScheduleEnabled(id) {
        var sid = (id === undefined || id === null) ? "" : ("" + id)
        if (sid.length === 0)
            return
        var enabled = true
        for (var i = 0; i < schedules.length; ++i) {
            if (("" + (schedules[i].scheduleId || schedules[i].id)) === sid) {
                enabled = !schedules[i].enabled
                break
            }
        }
        if (!serviceClient.setScheduleEnabled(sid, enabled)) {
            //% "Could not update schedule"
            serviceClient.showToast(qsTrId("aegra.backup.schedule.update_failed"))
        }
    }

    function deleteSchedule(id) {
        var sid = (id === undefined || id === null) ? "" : ("" + id)
        if (sid.length === 0)
            return
        if (!serviceClient.deleteSchedule(sid)) {
            //% "Could not delete schedule"
            serviceClient.showToast(qsTrId("aegra.backup.schedule.delete_failed"))
        }
    }

    // ==================== LIST ====================
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Row {
                spacing: 8
                Rectangle {
                    width: 3
                    height: 20
                    color: Theme.colorAccentBlue
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    //% "Backup"
                    text: qsTrId("aegra.nav.backup")
                    color: Theme.colorTextWhite
                    font.pixelSize: 18
                    font.bold: true
                    font.family: Theme.fontFamily
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Item { Layout.fillWidth: true }
        }

        Card {
            Layout.fillWidth: true
            Layout.fillHeight: true
            //% "Schedules"
            title: qsTrId("aegra.backup.section.schedule")

            AppButton {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.top: parent.top
                anchors.topMargin: 8
                z: 2
                //% "Add"
                text: qsTrId("aegra.common.add")
                onClicked: root.openWizard()
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 44
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                anchors.bottomMargin: 12
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    color: Theme.colorTableHeader
                    radius: 2
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 8
                        Text {
                            Layout.preferredWidth: 140
                            Layout.fillWidth: true
                            //% "Source"
                            text: qsTrId("aegra.backup.section.source")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 100
                            Layout.fillWidth: true
                            //% "Destination"
                            text: qsTrId("aegra.backup.column.destination")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 110
                            //% "Frequency"
                            text: qsTrId("aegra.backup.column.frequency")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 130
                            //% "Last run"
                            text: qsTrId("aegra.backup.column.last_run")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 130
                            //% "Next run"
                            text: qsTrId("aegra.backup.column.next_run")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 72
                            //% "Enabled"
                            text: qsTrId("aegra.backup.column.enabled")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Item { Layout.preferredWidth: 40 }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Text {
                        anchors.centerIn: parent
                        visible: root.schedules.length === 0
                        //% "No schedules"
                        text: qsTrId("aegra.backup.schedules.empty")
                        color: Theme.colorTextGrey
                        font.pixelSize: 13
                        font.family: Theme.fontFamily
                    }

                    ListView {
                        id: scheduleList
                        anchors.fill: parent
                        clip: true
                        spacing: 0
                        visible: root.schedules.length > 0
                        model: root.schedules

                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            width: scheduleList.width
                            height: (modelData.destinationPath || "") !== "" ? 52 : 44
                            color: index % 2 === 0 ? Theme.colorTableRow : Theme.colorTableAlt

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 8

                                Text {
                                    Layout.preferredWidth: 140
                                    Layout.fillWidth: true
                                    text: modelData.sourceName || ""
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideMiddle
                                }
                                Item {
                                    Layout.preferredWidth: 120
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Column {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        spacing: 0
                                        Text {
                                            width: parent.width
                                            height: (modelData.destinationPath || "") !== ""
                                                    ? 16 : implicitHeight
                                            text: modelData.destinationName || ""
                                            color: Theme.colorTextWhite
                                            font.pixelSize: (modelData.destinationPath || "") !== ""
                                                            ? 11 : 12
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideMiddle
                                        }
                                        Text {
                                            width: parent.width
                                            height: 14
                                            visible: (modelData.destinationPath || "") !== ""
                                            text: modelData.destinationPath || ""
                                            color: Theme.colorTextGrey
                                            font.pixelSize: 9
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideMiddle
                                        }
                                    }
                                }
                                Text {
                                    Layout.preferredWidth: 110
                                    text: root.freqLabel(modelData.frequency)
                                          + " · " + (modelData.timeOfDay || "02:00")
                                    color: Theme.colorAccentBlue
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.preferredWidth: 130
                                    text: root.timeOrNa(modelData.lastRun)
                                    color: (modelData.lastRun && ("" + modelData.lastRun).trim().length > 0)
                                           ? Theme.colorTextWhite : Theme.colorTextGrey
                                    font.pixelSize: 11
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.preferredWidth: 130
                                    text: root.timeOrNa(modelData.nextRun)
                                    color: (modelData.nextRun && ("" + modelData.nextRun).trim().length > 0)
                                           ? Theme.colorTextWhite : Theme.colorTextGrey
                                    font.pixelSize: 11
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                }
                                Item {
                                    Layout.preferredWidth: 72
                                    Layout.fillHeight: true
                                    Rectangle {
                                        width: 40
                                        height: 22
                                        radius: 11
                                        anchors.centerIn: parent
                                        color: modelData.enabled ? Theme.colorAccentBlue : "#555"
                                        border.width: 1
                                        border.color: modelData.enabled
                                                      ? Theme.colorAccentBlue : Theme.colorBorder
                                        Rectangle {
                                            width: 16
                                            height: 16
                                            radius: 8
                                            anchors.verticalCenter: parent.verticalCenter
                                            x: modelData.enabled ? parent.width - width - 3 : 3
                                            color: "white"
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.toggleScheduleEnabled(modelData.id)
                                        }
                                    }
                                }
                                Item {
                                    Layout.preferredWidth: 40
                                    Layout.fillHeight: true
                                    Rectangle {
                                        id: moreBtn
                                        width: 32
                                        height: 28
                                        radius: 4
                                        anchors.centerIn: parent
                                        color: (moreHover.containsMouse || scheduleMenu.visible)
                                               ? Theme.colorButtonHover : Theme.colorButton
                                        border.width: 1
                                        border.color: Theme.colorBorder
                                        //% "More actions"
                                        Accessible.name: qsTrId("aegra.backup.action.more")
                                        Text {
                                            anchors.centerIn: parent
                                            // Vertical ellipsis ⋮
                                            text: "\u22EE"
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 16
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                        }
                                        MouseArea {
                                            id: moreHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: scheduleMenu.open()
                                        }
                                        Menu {
                                            id: scheduleMenu
                                            y: moreBtn.height + 2
                                            x: moreBtn.width - width
                                            padding: 4
                                            background: Rectangle {
                                                implicitWidth: 168
                                                color: Theme.colorPopup
                                                border.width: 1
                                                border.color: Theme.colorBorder
                                                radius: 4
                                            }
                                            MenuItem {
                                                //% "Run full"
                                                text: qsTrId("aegra.backup.action.run_full")
                                                enabled: modelData.enabled
                                                height: 32
                                                leftPadding: 12
                                                rightPadding: 12
                                                background: Rectangle {
                                                    color: parent.highlighted
                                                           ? Theme.colorHover : "transparent"
                                                    radius: 3
                                                    opacity: parent.enabled ? 1.0 : 0.45
                                                }
                                                contentItem: Text {
                                                    text: parent.text
                                                    color: Theme.colorTextWhite
                                                    font.pixelSize: 12
                                                    font.family: Theme.fontFamily
                                                    verticalAlignment: Text.AlignVCenter
                                                    elide: Text.ElideRight
                                                }
                                                onTriggered: root.runSchedule(modelData, 1)
                                            }
                                            MenuItem {
                                                //% "Run incremental"
                                                text: qsTrId("aegra.backup.action.run_incremental")
                                                enabled: modelData.enabled
                                                height: 32
                                                leftPadding: 12
                                                rightPadding: 12
                                                background: Rectangle {
                                                    color: parent.highlighted
                                                           ? Theme.colorHover : "transparent"
                                                    radius: 3
                                                    opacity: parent.enabled ? 1.0 : 0.45
                                                }
                                                contentItem: Text {
                                                    text: parent.text
                                                    color: Theme.colorTextWhite
                                                    font.pixelSize: 12
                                                    font.family: Theme.fontFamily
                                                    verticalAlignment: Text.AlignVCenter
                                                    elide: Text.ElideRight
                                                }
                                                onTriggered: root.runSchedule(modelData, 2)
                                            }
                                            MenuSeparator {
                                                contentItem: Rectangle {
                                                    implicitHeight: 1
                                                    color: Theme.colorBorder
                                                }
                                                topPadding: 4
                                                bottomPadding: 4
                                            }
                                            MenuItem {
                                                //% "Delete"
                                                text: qsTrId("aegra.common.delete")
                                                height: 32
                                                leftPadding: 12
                                                rightPadding: 12
                                                background: Rectangle {
                                                    color: parent.highlighted
                                                           ? "#cc3333" : "transparent"
                                                    radius: 3
                                                }
                                                contentItem: Text {
                                                    text: parent.text
                                                    color: Theme.colorTextWhite
                                                    font.pixelSize: 12
                                                    font.family: Theme.fontFamily
                                                    verticalAlignment: Text.AlignVCenter
                                                    elide: Text.ElideRight
                                                }
                                                onTriggered: root.deleteSchedule(modelData.id)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ==================== WIZARD (old Add Schedule Wizard step 1) ====================
    Item {
        id: wizardDrawer
        anchors.fill: parent
        z: 2000

        Rectangle {
            anchors.fill: parent
            color: Theme.colorScrim
            opacity: root.wizardOpen ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 250 } }
            MouseArea {
                anchors.fill: parent
                enabled: root.wizardOpen
            }
        }

        Rectangle {
            id: wizardPanel
            width: Math.max(420, parent.width * 9 / 10)
            height: parent.height
            property real slideProgress: root.wizardOpen ? 0 : 1
            x: parent.width - width + slideProgress * width
            color: Theme.colorBg
            border.width: 1
            border.color: Theme.colorBorder
            Behavior on slideProgress {
                NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                // Header — "Add Schedule Wizard"
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    spacing: 8
                    Rectangle {
                        width: 3
                        height: 18
                        color: Theme.colorAccentBlue
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        //% "Add Schedule Wizard"
                        text: qsTrId("aegra.backup.wizard.title")
                        color: Theme.colorTextWhite
                        font.pixelSize: 16
                        font.bold: true
                        font.family: Theme.fontFamily
                        Layout.fillWidth: true
                        elide: Text.ElideRight
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
                        onClicked: root.closeWizard()
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: root.wizardStep === 1 ? 0 : 1

                    // -------- Step 1: SOURCE | DESTINATION --------
                    ColumnLayout {
                        spacing: 12

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 20

                            // -------- SOURCE (old disksTree: disk + expandable volumes) --------
                            Card {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                //% "SOURCE"
                                title: qsTrId("aegra.backup.section.source_upper")

                                ScrollView {
                                    id: sourceScroll
                                    anchors.fill: parent
                                    anchors.topMargin: 50
                                    anchors.margins: 16
                                    clip: true
                                    contentWidth: availableWidth

                                    Column {
                                        id: disksColumn
                                        width: sourceScroll.availableWidth
                                        spacing: 10

                                        Repeater {
                                            model: root.disksTree
                                            delegate: Column {
                                                id: diskDelegate
                                                width: disksColumn.width
                                                spacing: 0
                                                required property int index
                                                required property var modelData
                                                readonly property int diskIndex: index
                                                readonly property bool isExpanded:
                                                    root.isDiskExpanded(diskIndex)
                                                readonly property bool isSelectable:
                                                    modelData.selectable !== false
                                                readonly property var volumes:
                                                    modelData.volumes || []
                                                readonly property bool hasVolumes:
                                                    volumes.length > 0

                                                Rectangle {
                                                    width: diskDelegate.width
                                                    height: 45
                                                    radius: 4
                                                    color: diskHover.containsMouse && hasVolumes
                                                           ? Theme.colorHover : Theme.colorListItem
                                                    opacity: isSelectable ? 1.0 : 0.55

                                                    RowLayout {
                                                        anchors.fill: parent
                                                        anchors.leftMargin: 10
                                                        anchors.rightMargin: 10
                                                        spacing: 10

                                                        Rectangle {
                                                            width: 18
                                                            height: 18
                                                            radius: 3
                                                            visible: isSelectable
                                                            property bool checked:
                                                                root.isDiskSelected(diskIndex)
                                                            color: checked ? Theme.colorAccentBlue
                                                                           : "transparent"
                                                            border.width: 2
                                                            border.color: checked
                                                                          ? Theme.colorAccentBlue
                                                                          : Theme.colorTextGrey
                                                            Text {
                                                                anchors.centerIn: parent
                                                                text: parent.checked ? "\u2713" : ""
                                                                color: "white"
                                                                font.pixelSize: 12
                                                                font.bold: true
                                                            }
                                                            MouseArea {
                                                                anchors.fill: parent
                                                                cursorShape: Qt.PointingHandCursor
                                                                onClicked: root.toggleDiskSelected(
                                                                               diskIndex)
                                                            }
                                                        }
                                                        Item {
                                                            width: 18
                                                            height: 18
                                                            visible: !isSelectable
                                                        }

                                                        Text {
                                                            text: hasVolumes ? "\u25B6" : ""
                                                            color: Theme.colorTextGrey
                                                            font.pixelSize: 10
                                                            Layout.preferredWidth: 15
                                                            rotation: isExpanded ? 90 : 0
                                                            Behavior on rotation {
                                                                NumberAnimation { duration: 150 }
                                                            }
                                                            MouseArea {
                                                                anchors.fill: parent
                                                                enabled: hasVolumes
                                                                cursorShape: Qt.PointingHandCursor
                                                                onClicked: root.toggleDiskExpanded(
                                                                               diskIndex)
                                                            }
                                                        }

                                                        DiskIcon {
                                                            size: 28
                                                            variant: modelData.isSystemDisk
                                                                     ? "system" : "hdd"
                                                        }

                                                        ColumnLayout {
                                                            Layout.fillWidth: true
                                                            spacing: 2
                                                            Text {
                                                                text: (modelData.name || "")
                                                                      + (modelData.size
                                                                         ? (" (" + modelData.size
                                                                            + ")") : "")
                                                                color: Theme.colorTextWhite
                                                                font.pixelSize: 13
                                                                font.bold: true
                                                                font.family: Theme.fontFamily
                                                            }
                                                            Text {
                                                                text: modelData.type || "GPT"
                                                                color: Theme.colorTextGrey
                                                                font.pixelSize: 11
                                                                font.family: Theme.fontFamily
                                                            }
                                                        }
                                                        Item { Layout.fillWidth: true }
                                                    }
                                                    MouseArea {
                                                        id: diskHover
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        enabled: hasVolumes
                                                        cursorShape: hasVolumes
                                                                     ? Qt.PointingHandCursor
                                                                     : Qt.ArrowCursor
                                                        z: -1
                                                        onClicked: root.toggleDiskExpanded(diskIndex)
                                                    }
                                                }

                                                Column {
                                                    width: diskDelegate.width
                                                    spacing: 2
                                                    clip: true
                                                    height: isExpanded
                                                            ? (volumes.length * 57) : 0
                                                    opacity: isExpanded ? 1 : 0
                                                    Behavior on height {
                                                        NumberAnimation { duration: 150 }
                                                    }

                                                    Repeater {
                                                        model: volumes
                                                        delegate: Rectangle {
                                                            id: volumeDelegate
                                                            required property int index
                                                            required property var modelData
                                                            width: diskDelegate.width
                                                            height: 55
                                                            radius: 4
                                                            color: volHover.containsMouse
                                                                   ? Theme.colorHover
                                                                   : Theme.colorListItemAlt
                                                            readonly property int volumeIndex: index
                                                            readonly property bool isSelectable:
                                                                modelData.selectable === true
                                                            opacity: volumeDelegate.isSelectable
                                                                     ? 1.0 : 0.55

                                                            RowLayout {
                                                                anchors.fill: parent
                                                                anchors.leftMargin: 40
                                                                anchors.rightMargin: 10
                                                                spacing: 10
                                                                Rectangle {
                                                                    width: 16
                                                                    height: 16
                                                                    radius: 2
                                                                    property bool checked:
                                                                        root.isVolumeSelected(
                                                                            diskIndex, volumeIndex)
                                                                    color: checked
                                                                           ? Theme.colorAccentBlue
                                                                           : "transparent"
                                                                    border.width: 1
                                                                    border.color: checked
                                                                        ? Theme.colorAccentBlue
                                                                        : Theme.colorTextGrey
                                                                    Text {
                                                                        anchors.centerIn: parent
                                                                        text: parent.checked
                                                                              ? "\u2713" : ""
                                                                        color: "white"
                                                                        font.pixelSize: 10
                                                                        font.bold: true
                                                                    }
                                                                    MouseArea {
                                                                        anchors.fill: parent
                                                                        enabled:
                                                                            volumeDelegate.isSelectable
                                                                        cursorShape:
                                                                            volumeDelegate.isSelectable
                                                                            ? Qt.PointingHandCursor
                                                                            : Qt.ArrowCursor
                                                                        onClicked:
                                                                            root.toggleVolumeSelected(
                                                                                diskDelegate.diskIndex,
                                                                                volumeDelegate.volumeIndex)
                                                                    }
                                                                }
                                                                ColumnLayout {
                                                                    Layout.fillWidth: true
                                                                    spacing: 2
                                                                    RowLayout {
                                                                        spacing: 8
                                                                        Text {
                                                                            text: volumeDelegate.modelData.name
                                                                                  || ""
                                                                            color:
                                                                                Theme.colorTextWhite
                                                                            font.pixelSize: 12
                                                                            font.bold: true
                                                                            font.family:
                                                                                Theme.fontFamily
                                                                        }
                                                                        Text {
                                                                            text: volumeDelegate.modelData.letter
                                                                                  || ""
                                                                            color:
                                                                                Theme.colorAccentBlue
                                                                            font.pixelSize: 12
                                                                            font.family:
                                                                                Theme.fontFamily
                                                                            visible:
                                                                                (volumeDelegate.modelData.letter
                                                                                 || "").length > 0
                                                                        }
                                                                        Text {
                                                                            text: volumeDelegate.modelData.size
                                                                                  || ""
                                                                            color:
                                                                                Theme.colorTextGrey
                                                                            font.pixelSize: 11
                                                                            font.family:
                                                                                Theme.fontFamily
                                                                        }
                                                                    }
                                                                    Text {
                                                                        text: volumeDelegate.modelData.status
                                                                              || ""
                                                                        color: Theme.colorTextGrey
                                                                        font.pixelSize: 10
                                                                        font.family:
                                                                            Theme.fontFamily
                                                                        elide: Text.ElideRight
                                                                        Layout.fillWidth: true
                                                                    }
                                                                }
                                                            }
                                                            MouseArea {
                                                                id: volHover
                                                                anchors.fill: parent
                                                                hoverEnabled: true
                                                                enabled: volumeDelegate.isSelectable
                                                                cursorShape: volumeDelegate.isSelectable
                                                                             ? Qt.PointingHandCursor
                                                                             : Qt.ArrowCursor
                                                                z: -1
                                                                onClicked:
                                                                    root.toggleVolumeSelected(
                                                                        diskDelegate.diskIndex,
                                                                        volumeDelegate.volumeIndex)
                                                            }
                                                        }
                                                    }
                                                }
                                                Item { width: parent.width; height: 5 }
                                            }
                                        }
                                    }
                                }
                            }

                            // -------- DESTINATION --------
                            Card {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                //% "DESTINATION"
                                title: qsTrId("aegra.backup.section.destination_upper")

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.topMargin: 50
                                    anchors.margins: 16
                                    spacing: 10

                                    Text {
                                        //% "Locations"
                                        text: qsTrId("aegra.backup.locations")
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 11
                                        font.family: Theme.fontFamily
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        color: Theme.colorBg
                                        radius: 4
                                        border.width: 1
                                        border.color: Theme.colorBorder

                                        ListView {
                                            id: locList
                                            anchors.fill: parent
                                            anchors.margins: 4
                                            clip: true
                                            spacing: 2
                                            // Real Service repository.connection list only.
                                            model: serviceClient.connections

                                            // Empty Locations: keep Next disabled (canGoNext).
                                            Text {
                                                anchors.centerIn: parent
                                                width: parent.width - 24
                                                horizontalAlignment: Text.AlignHCenter
                                                wrapMode: Text.WordWrap
                                                visible: !serviceClient.connections
                                                         || serviceClient.connections.count === 0
                                                //% "No repository connection yet. Add a location in Repository first."
                                                text: qsTrId("aegra.backup.destination.empty")
                                                color: Theme.colorTextDim
                                                font.pixelSize: 12
                                                font.family: Theme.fontFamily
                                            }

                                            delegate: Rectangle {
                                                id: locRow
                                                width: Math.max(0, locList.width - 8)
                                                height: 52
                                                anchors.horizontalCenter: parent
                                                                          ? parent.horizontalCenter
                                                                          : undefined
                                                radius: 4
                                                color: locHover.containsMouse
                                                       ? Theme.colorHover : "transparent"

                                                required property int index
                                                required property var model
                                                property var modelData

                                                readonly property bool liveModel: true
                                                readonly property string locName: {
                                                    if (liveModel)
                                                        return (model.displayName
                                                                || model.connectionId || "")
                                                    return (modelData && modelData.name)
                                                           ? modelData.name : ""
                                                }
                                                readonly property string locPath: {
                                                    if (liveModel)
                                                        return model.connectionId || ""
                                                    return (modelData && modelData.path)
                                                           ? modelData.path : ""
                                                }
                                                readonly property bool locDefault: {
                                                    if (liveModel)
                                                        return !!model.isDefault
                                                    return !!(modelData && modelData.isDefault)
                                                }
                                                readonly property string locState: {
                                                    if (liveModel)
                                                        return model.stateText || ""
                                                    return ""
                                                }
                                                readonly property bool selected:
                                                    root.selectedLocationIndex === index

                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 8
                                                    anchors.rightMargin: 8
                                                    spacing: 8

                                                    Rectangle {
                                                        width: 18
                                                        height: 18
                                                        radius: 3
                                                        color: locRow.selected
                                                               ? Theme.colorAccentBlue
                                                               : "transparent"
                                                        border.width: 2
                                                        border.color: locRow.selected
                                                                      ? Theme.colorAccentBlue
                                                                      : Theme.colorTextGrey
                                                        Text {
                                                            anchors.centerIn: parent
                                                            text: locRow.selected ? "\u2713" : ""
                                                            color: "white"
                                                            font.pixelSize: 11
                                                            font.bold: true
                                                        }
                                                    }

                                                    Rectangle {
                                                        width: 24
                                                        height: 24
                                                        radius: 4
                                                        color: "#4a9eff"
                                                        Text {
                                                            anchors.centerIn: parent
                                                            text: "\uD83D\uDCBB"
                                                            font.pixelSize: 12
                                                        }
                                                    }

                                                    Column {
                                                        Layout.fillWidth: true
                                                        Layout.minimumWidth: 80
                                                        spacing: 1
                                                        Text {
                                                            width: parent.width
                                                            text: locRow.locName.length > 0
                                                                  ? locRow.locName
                                                                  : qsTrId("aegra.repository.title")
                                                            color: Theme.colorTextWhite
                                                            font.pixelSize: 12
                                                            font.bold: true
                                                            font.family: Theme.fontFamily
                                                            elide: Text.ElideRight
                                                        }
                                                        Text {
                                                            width: parent.width
                                                            text: locRow.locPath
                                                            color: Theme.colorTextGrey
                                                            font.pixelSize: 11
                                                            font.family: Theme.fontFamily
                                                            elide: Text.ElideMiddle
                                                            visible: locRow.locPath.length > 0
                                                        }
                                                        Text {
                                                            width: parent.width
                                                            text: locRow.locState
                                                            color: Theme.colorTextDim
                                                            font.pixelSize: 10
                                                            font.family: Theme.fontFamily
                                                            visible: locRow.locState.length > 0
                                                        }
                                                    }

                                                    Text {
                                                        text: locRow.locDefault
                                                              ? "\u2605" : "\u2606"
                                                        color: locRow.locDefault
                                                               ? Theme.colorAccentBlue
                                                               : Theme.colorTextDim
                                                        font.pixelSize: 14
                                                        opacity: 0.85
                                                    }
                                                    Text {
                                                        text: "\uD83D\uDDD1"
                                                        color: Theme.colorTextDim
                                                        font.pixelSize: 13
                                                        opacity: 0.5
                                                    }
                                                }

                                                MouseArea {
                                                    id: locHover
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: root.selectedLocationIndex = index
                                                    z: -1
                                                }
                                            }
                                        }
                                    }

                                    LinkButton {
                                        //% "Add location"
                                        text: qsTrId("aegra.backup.add_location")
                                        onClicked: { /* UI only for now */ }
                                    }
                                }
                            }
                        }

                        // Footer: Cancel + Next (wizard step 1)
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 15
                            Item { Layout.fillWidth: true }

                            AppButton {
                                //% "Cancel"
                                text: qsTrId("aegra.common.cancel")
                                Layout.preferredWidth: 100
                                Layout.preferredHeight: 40
                                onClicked: root.closeWizard()
                            }
                            AppButton {
                                //% "Next"
                                text: qsTrId("aegra.common.next")
                                Layout.preferredWidth: 140
                                Layout.preferredHeight: 40
                                enabled: root.canGoNext()
                                primary: true
                                onClicked: root.wizardStep = 2
                            }
                        }
                    }

                    // -------- Step 2: Schedule settings + Options --------
                    BackupWizardStep2 {
                        id: wizardStep2
                        onBackRequested: root.wizardStep = 1
                        onCreateRequested: root.createScheduleFromWizard()
                    }
                }
            }
        }
    }

}
