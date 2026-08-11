import QtQuick 2.15
import QtQuick.Window 2.15
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

    // File-tree row icons (icon before name; volume roots use disk glyph).
    Component {
        id: volumeIconComponent
        DiskIcon { size: 16; variant: "hdd" }
    }
    Component {
        id: folderIconComponent
        FolderIcon { size: 16 }
    }
    Component {
        id: fileIconComponent
        FileDocIcon { size: 16 }
    }
    Component {
        id: specialDesktopIconComponent
        SpecialFolderIcon { size: 16; variant: "desktop" }
    }
    Component {
        id: specialDownloadsIconComponent
        SpecialFolderIcon { size: 16; variant: "downloads" }
    }
    Component {
        id: specialDocumentsIconComponent
        SpecialFolderIcon { size: 16; variant: "documents" }
    }
    Component {
        id: specialPicturesIconComponent
        SpecialFolderIcon { size: 16; variant: "pictures" }
    }
    Component {
        id: specialMusicIconComponent
        SpecialFolderIcon { size: 16; variant: "music" }
    }
    Component {
        id: specialVideosIconComponent
        SpecialFolderIcon { size: 16; variant: "videos" }
    }

    /// Volume roots look like "Label (C:)" / "C:"; special folders use fixed English labels.
    function isVolumeRootLabel(name) {
        var text = String(name || "")
        return /\([A-Za-z]:\)$/.test(text) || /^[A-Za-z]:$/.test(text)
    }
    function specialFolderIconFor(name) {
        switch (String(name || "").toLowerCase()) {
        case "desktop": return specialDesktopIconComponent
        case "downloads": return specialDownloadsIconComponent
        case "documents": return specialDocumentsIconComponent
        case "pictures": return specialPicturesIconComponent
        case "music": return specialMusicIconComponent
        case "videos": return specialVideosIconComponent
        default: return null
        }
    }
    function fileSourceIconFor(depth, isDirectory, displayName) {
        if (depth === 0) {
            var special = specialFolderIconFor(displayName)
            if (special)
                return special
            if (isVolumeRootLabel(displayName))
                return volumeIconComponent
            return folderIconComponent
        }
        if (isDirectory)
            return folderIconComponent
        return fileIconComponent
    }

    // Add Schedule wizard (drawer)
    property bool wizardOpen: false
    property int wizardStep: 0
    /// "disk" | "files" — chosen on wizard step 0
    property string backupMode: "disk"
    property int selectedLocationIndex: 0
    property var expandedDisks: ({})
    property var selectedVolumeKeys: ({})
    property int selectionEpoch: 0

    // Staggered Entrance Animation States
    property bool animStage1: false
    property bool animStage2: false

    function restartEntranceAnimation() {
        animStage1 = false
        animStage2 = false
        t1.restart()
    }

    Timer { id: t1; interval: 60;  repeat: false; onTriggered: root.animStage1 = true }
    Timer { id: t2; interval: 180; repeat: false; onTriggered: root.animStage2 = true }

    onAnimStage1Changed: if (animStage1) t2.restart()

    Component.onCompleted: restartEntranceAnimation()

    onVisibleChanged: {
        if (visible) {
            restartEntranceAnimation()
        } else if (root.wizardOpen) {
            root.closeWizard()
        }
    }

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
        root.expandedDisks = next
    }

    function openWizard() {
        // Open drawer first so UI always reacts even if later setup throws.
        root.wizardOpen = true
        root.wizardStep = 0
        root.backupMode = "disk"
        root.selectedLocationIndex = 0
        root.expandedDisks = ({})
        root.selectedVolumeKeys = ({})
        root.selectionEpoch = 0
        if (typeof wizardStep2 !== "undefined" && wizardStep2)
            wizardStep2.resetDefaults()
        if (typeof serviceClient !== "undefined" && serviceClient && serviceClient.connected) {
            serviceClient.refreshInventory()
            serviceClient.refreshConnections()
            serviceClient.refreshSchedules()
        }
    }

    function closeWizard() {
        root.wizardOpen = false
        root.wizardStep = 0
    }

    // Step labels for progress header (wizard step 0 / 1 / 2)
    readonly property var wizardStepLabels: [
        qsTrId("aegra.backup.wizard.step.type"),
        qsTrId("aegra.backup.wizard.step.source_dest"),
        qsTrId("aegra.backup.wizard.step.options")
    ]

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
        if (selectedConnectionId().length === 0)
            return false
        if (root.backupMode === "files") {
            var tree = serviceClient.fileBrowseSources
            return !!(tree && tree.selectedCount > 0)
        }
        return selectedSources().length > 0
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

    /// Soft badge color for schedule row icons (design mini-badge palette).
    function scheduleBadgeColor(index) {
        var palette = ["#2A7982", "#3B82F6", "#10B981", "#8B5CF6", "#F59E0B", "#EF4444", "#0EA5E9"]
        var i = index % palette.length
        if (i < 0)
            i += palette.length
        return palette[i]
    }

    /// Job-list revision + active progress so Status cells rebind while a backup runs.
    readonly property int jobsStatusRevision: {
        var jobs = serviceClient.jobs
        var rev = jobs && jobs.revision !== undefined ? jobs.revision : 0
        var pct = serviceClient.activeBackupProgressPercent || 0
        var active = jobs && jobs.activeCount !== undefined ? jobs.activeCount : 0
        return rev * 10000 + active * 1000 + pct
    }

    /// Resolve backup status for a schedule row (success / failed / running+%).
    /// Bound by schedule_id so identical sources do not share one Job's state.
    /// Returns { statusKey, progressPercent, stateText }.
    function scheduleBackupStatus(schedule) {
        var _ = root.jobsStatusRevision
        if (!schedule)
            return { statusKey: "none", progressPercent: 0, stateText: "" }
        var jobs = serviceClient.jobs
        if (!jobs || typeof jobs.latestBackupStatus !== "function")
            return { statusKey: "none", progressPercent: 0, stateText: "" }
        var scheduleId = schedule.scheduleId || schedule.id || ""
        if (scheduleId.length === 0)
            return { statusKey: "none", progressPercent: 0, stateText: "" }
        var st = jobs.latestBackupStatus(scheduleId)
        if (!st)
            return { statusKey: "none", progressPercent: 0, stateText: "" }
        return {
            statusKey: st.statusKey || "none",
            progressPercent: st.progressPercent || 0,
            stateText: st.stateText || ""
        }
    }

    /// Pending wizard create payload while the first-backup confirm dialog is open.
    property var pendingCreatePayload: null

    function createScheduleFromWizard() {
        var filesMode = root.backupMode === "files"
        var sources = filesMode ? [] : selectedSources()
        var connId = selectedConnectionId()
        if (connId.length === 0 && serviceClient.defaultConnectionId)
            connId = serviceClient.defaultConnectionId() || ""
        if (filesMode) {
            var tree = serviceClient.fileBrowseSources
            if (!tree || tree.selectedCount <= 0) {
                //% "Select at least one backup source"
                serviceClient.showToast(qsTrId("aegra.backup.schedule.missing_source"), true)
                return
            }
        } else if (sources.length === 0) {
            //% "Select at least one backup source"
            serviceClient.showToast(qsTrId("aegra.backup.schedule.missing_source"), true)
            return
        }
        if (!connId || connId.length === 0) {
            //% "Select a repository destination (Locations)"
            serviceClient.showToast(qsTrId("aegra.backup.schedule.missing_target"), true)
            return
        }
        var s2 = (typeof wizardStep2 !== "undefined") ? wizardStep2 : null
        var frequency = s2 ? s2.frequency : "daily"
        var timeOfDay = s2 ? s2.timeOfDay : "02:00"
        var backupType = s2 ? (s2.backupType === 2 ? 2 : 1) : 1
        var excludePage = s2 ? s2.excludePageHibernation : true
        // volume_set only; file_set always false (ADR-0022).
        var enableDedup = filesMode ? false : (s2 ? s2.enableDedup : true)
        var encryption = s2 ? s2.encryption : false
        var password = s2 ? (s2.password || "") : ""
        var passwordConfirm = s2 ? (s2.passwordConfirm || "") : ""
        if (encryption) {
            if (password.length === 0 || password.length > 32) {
                serviceClient.showToast(qsTrId("aegra.backup.opt.password_required"), true)
                return
            }
            if (password !== passwordConfirm) {
                serviceClient.showToast(qsTrId("aegra.backup.opt.password_mismatch"), true)
                return
            }
        }
        // Ask whether to run the first Full backup immediately, then create.
        root.pendingCreatePayload = {
            filesMode: filesMode,
            sources: sources,
            connId: connId,
            frequency: frequency,
            timeOfDay: timeOfDay,
            backupType: backupType,
            excludePage: excludePage,
            enableDedup: enableDedup,
            encryption: encryption,
            password: encryption ? password : ""
        }
        firstBackupConfirm.open()
    }

    function commitPendingCreate(startFirstBackup) {
        var p = root.pendingCreatePayload
        root.pendingCreatePayload = null
        if (!p)
            return
        var ok = false
        if (p.filesMode) {
            ok = serviceClient.createFileSetSchedule(p.connId, p.frequency, p.timeOfDay,
                                                     p.excludePage, p.encryption, p.password,
                                                     !!startFirstBackup,
                                                     (p.backupType === 2) ? 2 : 1)
        } else {
            ok = serviceClient.createSchedule(p.sources, p.connId, p.frequency, p.timeOfDay,
                                              p.excludePage, !!p.enableDedup, p.encryption,
                                              p.password, !!startFirstBackup,
                                              (p.backupType === 2) ? 2 : 1)
        }
        if (!ok) {
            //% "Could not save schedule"
            serviceClient.showToast(qsTrId("aegra.backup.schedule.save_failed"), true)
            return
        }
        // Stay on Backup / Schedules page after create (do not jump to Home).
        closeWizard()
    }

    function cancelPendingCreate() {
        root.pendingCreatePayload = null
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
            // Stay on Schedules page after a backup starts (no auto-navigate to Home).
        }
        function onBackupStartFailed(message) {
            root.pendingRunScheduleId = ""
        }
        function onSchedulesChanged() {
            // Schedules reloaded from Service after create/toggle/delete.
        }
    }

    // Confirm: start first Full backup immediately after creating the schedule?
    Popup {
        id: firstBackupConfirm
        parent: Overlay.overlay
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        anchors.centerIn: Overlay.overlay
        width: Math.min(420, Overlay.overlay ? Overlay.overlay.width - 48 : 420)
        padding: 20
        /// When true, onClosed must not drop the pending create (button already committed).
        property bool committing: false

        onClosed: {
            firstBackupConfirm.committing = false
            // Escape dismiss without choosing — drop pending create, keep wizard open.
            if (root.pendingCreatePayload)
                root.cancelPendingCreate()
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
                //% "Start first backup?"
                text: qsTrId("aegra.backup.schedule.first_backup_title")
                color: Theme.colorTextWhite
                font.pixelSize: 16
                font.bold: true
                font.family: Theme.fontFamily
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                //% "The schedule will be saved. Do you want to start the first full backup now?"
                text: qsTrId("aegra.backup.schedule.first_backup_message")
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
                    //% "Later"
                    text: qsTrId("aegra.backup.schedule.first_backup_later")
                    Layout.preferredHeight: 36
                    onClicked: {
                        firstBackupConfirm.committing = true
                        root.commitPendingCreate(false)
                        firstBackupConfirm.close()
                    }
                }
                AppButton {
                    //% "Start now"
                    text: qsTrId("aegra.backup.schedule.first_backup_now")
                    primary: true
                    Layout.preferredHeight: 36
                    onClicked: {
                        firstBackupConfirm.committing = true
                        root.commitPendingCreate(true)
                        firstBackupConfirm.close()
                    }
                }
            }
        }
    }

    // Confirm before permanently removing a schedule.
    Popup {
        id: deleteScheduleConfirm
        parent: Overlay.overlay
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        anchors.centerIn: Overlay.overlay
        width: Math.min(420, Overlay.overlay ? Overlay.overlay.width - 48 : 420)
        padding: 20
        property bool committing: false

        onClosed: {
            if (!deleteScheduleConfirm.committing)
                root.cancelDeleteSchedule()
            deleteScheduleConfirm.committing = false
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
                //% "Delete schedule?"
                text: qsTrId("aegra.backup.schedule.delete_confirm_title")
                color: Theme.colorTextWhite
                font.pixelSize: 16
                font.bold: true
                font.family: Theme.fontFamily
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                //% "This removes the schedule only. Existing recovery points in the repository are not deleted."
                text: qsTrId("aegra.backup.schedule.delete_confirm_message")
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
                        deleteScheduleConfirm.committing = true
                        root.cancelDeleteSchedule()
                        deleteScheduleConfirm.close()
                    }
                }
                AppButton {
                    //% "Delete"
                    text: qsTrId("aegra.common.delete")
                    primary: true
                    Layout.preferredHeight: 36
                    onClicked: {
                        deleteScheduleConfirm.committing = true
                        root.confirmDeleteSchedule()
                        deleteScheduleConfirm.close()
                    }
                }
            }
        }
    }

    /// Run schedule now. backupType: 1 = full, 2 = incremental (service_protocol).
    function runSchedule(item, backupType) {
        if (!item || !item.enabled) {
            //% "Enable the schedule before running it"
            serviceClient.showToast(qsTrId("aegra.backup.run.disabled"), true)
            return
        }
        var type = (backupType === 2) ? 2 : 1
        if (serviceClient.connected && serviceClient.hasCapability("backup.start")) {
            var scheduleId = item.scheduleId || item.id || ""
            if (scheduleId.length > 0) {
                root.pendingRunScheduleId = scheduleId
                if (serviceClient.startBackup(scheduleId, type))
                    return
                root.pendingRunScheduleId = ""
                return
            }
            //% "No selectable source or repository connection for backup"
            serviceClient.showToast(qsTrId("aegra.backup.run.missing_target"), true)
            return
        }
        //% "Service not connected"
        serviceClient.showToast(qsTrId("aegra.backup.run.not_connected"), true)
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
            serviceClient.showToast(qsTrId("aegra.backup.schedule.update_failed"), true)
        }
    }

    property string pendingDeleteScheduleId: ""

    /// Open confirm dialog before deleting a schedule (menu → Delete).
    function requestDeleteSchedule(id) {
        var sid = (id === undefined || id === null) ? "" : ("" + id)
        if (sid.length === 0)
            return
        root.pendingDeleteScheduleId = sid
        deleteScheduleConfirm.open()
    }

    function confirmDeleteSchedule() {
        var sid = root.pendingDeleteScheduleId
        root.pendingDeleteScheduleId = ""
        if (sid.length === 0)
            return
        if (!serviceClient.deleteSchedule(sid)) {
            //% "Could not delete schedule"
            serviceClient.showToast(qsTrId("aegra.backup.schedule.delete_failed"), true)
        }
    }

    function cancelDeleteSchedule() {
        root.pendingDeleteScheduleId = ""
    }

    // ==================== LIST ====================
    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        anchors.bottomMargin: 24
        anchors.topMargin: 40
        spacing: 16

        // ===============================================
        // TOP STAT METRIC CARDS (3 Cards Row)
        // ===============================================
        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            // Stat 1: Total Backup Runs
            Card {
                Layout.fillWidth: true
                implicitHeight: 92

                opacity: root.animStage1 ? 1 : 0
                transform: Translate { y: root.animStage1 ? 0 : 36 }
                scale: root.animStage1 ? 1.0 : 0.95
                Behavior on opacity { NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }
                Behavior on transform { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
                Behavior on scale { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 14

                    Rectangle {
                        id: iconBox1
                        width: 44
                        height: 44
                        radius: 14
                        transformOrigin: Item.Center
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#3B82F6" }
                            GradientStop { position: 1.0; color: "#2563EB" }
                        }
                        Text { anchors.centerIn: parent; text: "🔄"; font.pixelSize: 20 }

                        ParallelAnimation {
                            running: root.animStage1
                            NumberAnimation {
                                target: iconBox1
                                property: "scale"
                                from: 0.2
                                to: 1.0
                                duration: 650
                                easing.type: Easing.OutBack
                                easing.overshoot: 1.8
                            }
                            NumberAnimation {
                                target: iconBox1
                                property: "rotation"
                                from: -25
                                to: 0
                                duration: 650
                                easing.type: Easing.OutBack
                                easing.overshoot: 1.8
                            }
                        }
                    }

                    ColumnLayout {
                        spacing: 2
                        Text {
                            //% "Total backup runs"
                            text: qsTrId("aegra.backup.stat.total_runs")
                            color: Theme.colorTextGrey
                            font.pixelSize: 10
                            font.bold: true
                        }
                        Text {
                            //% "%1 runs"
                            text: qsTrId("aegra.backup.stat.total_runs_value").arg(48)
                            color: Theme.colorTextWhite
                            font.pixelSize: 20
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }
                }
            }

            // Stat 2: Days Protected
            Card {
                Layout.fillWidth: true
                implicitHeight: 92

                opacity: root.animStage1 ? 1 : 0
                transform: Translate { y: root.animStage1 ? 0 : 36 }
                scale: root.animStage1 ? 1.0 : 0.95
                Behavior on opacity { NumberAnimation { duration: 420; easing.type: Easing.OutCubic } }
                Behavior on transform { NumberAnimation { duration: 540; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
                Behavior on scale { NumberAnimation { duration: 540; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 14

                    Rectangle {
                        id: iconBox2
                        width: 44
                        height: 44
                        radius: 14
                        transformOrigin: Item.Center
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#10B981" }
                            GradientStop { position: 1.0; color: "#059669" }
                        }
                        Text { anchors.centerIn: parent; text: "⏱️"; font.pixelSize: 20 }

                        ParallelAnimation {
                            running: root.animStage1
                            NumberAnimation {
                                target: iconBox2
                                property: "scale"
                                from: 0.2
                                to: 1.0
                                duration: 680
                                easing.type: Easing.OutBack
                                easing.overshoot: 1.8
                            }
                            NumberAnimation {
                                target: iconBox2
                                property: "rotation"
                                from: -25
                                to: 0
                                duration: 680
                                easing.type: Easing.OutBack
                                easing.overshoot: 1.8
                            }
                        }
                    }

                    ColumnLayout {
                        spacing: 2
                        Text {
                            //% "Since first backup"
                            text: qsTrId("aegra.backup.stat.since_first")
                            color: Theme.colorTextGrey
                            font.pixelSize: 10
                            font.bold: true
                        }
                        Text {
                            //% "%1 days"
                            text: qsTrId("aegra.backup.stat.since_first_value").arg(126)
                            color: Theme.colorTextWhite
                            font.pixelSize: 20
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }
                }
            }

            // Stat 3: Schedules Count
            Card {
                Layout.fillWidth: true
                implicitHeight: 92

                opacity: root.animStage1 ? 1 : 0
                transform: Translate { y: root.animStage1 ? 0 : 36 }
                scale: root.animStage1 ? 1.0 : 0.95
                Behavior on opacity { NumberAnimation { duration: 460; easing.type: Easing.OutCubic } }
                Behavior on transform { NumberAnimation { duration: 560; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
                Behavior on scale { NumberAnimation { duration: 560; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 14

                    Rectangle {
                        id: iconBox3
                        width: 44
                        height: 44
                        radius: 14
                        transformOrigin: Item.Center
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#8B5CF6" }
                            GradientStop { position: 1.0; color: "#7C3AED" }
                        }
                        Text { anchors.centerIn: parent; text: "⚡"; font.pixelSize: 20 }

                        ParallelAnimation {
                            running: root.animStage1
                            NumberAnimation {
                                target: iconBox3
                                property: "scale"
                                from: 0.2
                                to: 1.0
                                duration: 710
                                easing.type: Easing.OutBack
                                easing.overshoot: 1.8
                            }
                            NumberAnimation {
                                target: iconBox3
                                property: "rotation"
                                from: -25
                                to: 0
                                duration: 710
                                easing.type: Easing.OutBack
                                easing.overshoot: 1.8
                            }
                        }
                    }

                    ColumnLayout {
                        spacing: 2
                        Text {
                            //% "Scheduled tasks"
                            text: qsTrId("aegra.backup.stat.schedule_count")
                            color: Theme.colorTextGrey
                            font.pixelSize: 10
                            font.bold: true
                        }
                        Text {
                            //% "%1 schedules"
                            text: qsTrId("aegra.backup.stat.schedule_count_value").arg(root.schedules.length)
                            color: Theme.colorTextWhite
                            font.pixelSize: 20
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }
                }
            }
        }

        // ===============================================
        // SCHEDULES TABLE (CoachPro standings-style)
        // ===============================================
        Card {
            id: scheduleCard
            Layout.fillWidth: true
            Layout.fillHeight: true
            //% "Schedules"
            title: qsTrId("aegra.backup.section.schedule")

            opacity: root.animStage2 ? 1 : 0
            enabled: true
            transform: Translate { y: root.animStage2 ? 0 : 36 }
            scale: root.animStage2 ? 1.0 : 0.95
            Behavior on opacity { NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }
            Behavior on scale { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

            // Design card-action link (top-right), same role as "+ Add"
            Text {
                id: addScheduleButton
                z: 30
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 20
                anchors.rightMargin: 22
                //% "Add"
                text: "+ " + qsTrId("aegra.common.add")
                color: addHover.containsMouse ? Theme.colorLinkHover : Theme.colorAccentBlue
                font.pixelSize: 13
                font.bold: true
                font.family: Theme.fontFamily
                MouseArea {
                    id: addHover
                    anchors.fill: parent
                    anchors.margins: -6
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openWizard()
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 52
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                anchors.bottomMargin: 10
                spacing: 0
                z: 0

                // Column headers — uppercase, muted, letter-spaced (standings th)
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 4

                        Text {
                            Layout.preferredWidth: 28
                            text: "#"
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Text {
                            Layout.preferredWidth: 160
                            Layout.fillWidth: true
                            //% "SOURCE"
                            text: qsTrId("aegra.backup.section.source_upper")
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 120
                            Layout.fillWidth: true
                            //% "DESTINATION"
                            text: qsTrId("aegra.backup.section.destination_upper")
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Text {
                            Layout.preferredWidth: 100
                            //% "Frequency"
                            text: qsTrId("aegra.backup.column.frequency").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Text {
                            Layout.preferredWidth: 110
                            //% "Status"
                            text: qsTrId("aegra.backup.column.status").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Text {
                            Layout.preferredWidth: 110
                            //% "Next run"
                            text: qsTrId("aegra.backup.column.next_run").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Text {
                            Layout.preferredWidth: 56
                            //% "Enabled"
                            text: qsTrId("aegra.backup.column.enabled").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Item { Layout.preferredWidth: 36 }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 48
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        visible: root.schedules.length === 0
                        text: {
                            if (serviceClient.schedulesLoading)
                                return qsTrId("aegra.common.loading")
                            if (serviceClient.schedulesErrorText
                                    && serviceClient.schedulesErrorText.length > 0)
                                return serviceClient.schedulesErrorText
                            //% "No schedules"
                            return qsTrId("aegra.backup.schedules.empty")
                        }
                        color: (serviceClient.schedulesErrorText
                                && serviceClient.schedulesErrorText.length > 0)
                               ? Theme.colorAccentRed : Theme.colorTextGrey
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
                        boundsBehavior: Flickable.StopAtBounds

                        delegate: Item {
                            id: scheduleRow
                            required property var modelData
                            required property int index
                            width: scheduleList.width
                            height: 52

                            // Subtle top divider (standings border-top)
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                height: 1
                                color: Theme.colorBorder
                                opacity: 0.55
                            }

                            // HoverHandler does not steal events from child controls (toggle / menu),
                            // so moving over buttons no longer toggles containsMouse and flicker.
                            HoverHandler {
                                id: rowHover
                                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                            }

                            Rectangle {
                                id: rowBg
                                anchors.fill: parent
                                anchors.leftMargin: 4
                                anchors.rightMargin: 4
                                radius: 10
                                color: rowHover.hovered
                                       ? Theme.colorHover
                                       : "transparent"
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 4

                                // Rank #
                                Text {
                                    Layout.preferredWidth: 28
                                    text: "" + (scheduleRow.index + 1)
                                    color: Theme.colorTextDim
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }

                                // Source with mini-badge (TEAM cell)
                                Item {
                                    Layout.preferredWidth: 160
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true

                                    Row {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        spacing: 12

                                        Rectangle {
                                            id: miniBadge
                                            width: 28
                                            height: 28
                                            radius: 8
                                            color: Theme.colorHover
                                            border.width: 1
                                            border.color: Theme.colorBorder
                                            // Avoid transform-on-hover (can jitter layout under cursor)
                                            scale: 1.0

                                            Rectangle {
                                                anchors.centerIn: parent
                                                width: 18
                                                height: 18
                                                radius: 5
                                                color: root.scheduleBadgeColor(scheduleRow.index)
                                                opacity: 0.92
                                            }
                                            Text {
                                                anchors.centerIn: parent
                                                text: {
                                                    var n = (modelData.sourceName || "?").trim()
                                                    return n.length > 0 ? n.charAt(0).toUpperCase() : "?"
                                                }
                                                color: "#ffffff"
                                                font.pixelSize: 10
                                                font.bold: true
                                                font.family: Theme.fontFamily
                                            }
                                        }

                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: Math.max(40, scheduleRow.width * 0.18)
                                            text: modelData.sourceName || ""
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 14
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideMiddle
                                        }
                                    }
                                }

                                // Destination (muted centered stats)
                                Text {
                                    Layout.preferredWidth: 120
                                    Layout.fillWidth: true
                                    text: modelData.destinationName || ""
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideMiddle
                                }

                                // Frequency + scheduled backup type
                                Column {
                                    Layout.preferredWidth: 110
                                    spacing: 2
                                    Text {
                                        width: parent.width
                                        text: root.freqLabel(modelData.frequency)
                                              + " · " + (modelData.timeOfDay || "02:00")
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 13
                                        font.bold: true
                                        font.family: Theme.fontFamily
                                        horizontalAlignment: Text.AlignHCenter
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        width: parent.width
                                        text: {
                                            var t = modelData.backupType
                                            if (t === 2)
                                                return qsTrId("aegra.backup.type.incremental")
                                            return qsTrId("aegra.backup.type.full")
                                        }
                                        color: Theme.colorTextDim
                                        font.pixelSize: 11
                                        font.family: Theme.fontFamily
                                        horizontalAlignment: Text.AlignHCenter
                                        elide: Text.ElideRight
                                    }
                                }

                                // Status: success / failed / running+progress%
                                Item {
                                    id: statusCell
                                    Layout.preferredWidth: 110
                                    Layout.fillHeight: true
                                    readonly property var backupStatus: root.scheduleBackupStatus(modelData)
                                    readonly property string statusKey: backupStatus.statusKey || "none"
                                    readonly property int progressPct: backupStatus.progressPercent || 0

                                    Row {
                                        anchors.centerIn: parent
                                        spacing: 6

                                        // Icon badge: ✓ success · ⚠ failed · ↻↻ running · – none
                                        Item {
                                            id: statusIcon
                                            width: 22
                                            height: 22
                                            anchors.verticalCenter: parent.verticalCenter
                                            property real spinAngle: 0
                                            rotation: statusCell.statusKey === "running" ? spinAngle : 0

                                            Rectangle {
                                                anchors.fill: parent
                                                radius: width / 2
                                                color: {
                                                    if (statusCell.statusKey === "success")
                                                        return Qt.rgba(Theme.colorGreen.r,
                                                                       Theme.colorGreen.g,
                                                                       Theme.colorGreen.b, 0.18)
                                                    if (statusCell.statusKey === "failed")
                                                        return Qt.rgba(Theme.colorAccentRed.r,
                                                                       Theme.colorAccentRed.g,
                                                                       Theme.colorAccentRed.b, 0.18)
                                                    if (statusCell.statusKey === "running")
                                                        return Qt.rgba(Theme.colorAccentBlue.r,
                                                                       Theme.colorAccentBlue.g,
                                                                       Theme.colorAccentBlue.b, 0.18)
                                                    return Theme.colorProgressTrack
                                                }
                                            }
                                            Text {
                                                anchors.centerIn: parent
                                                visible: statusCell.statusKey !== "running"
                                                text: {
                                                    if (statusCell.statusKey === "success")
                                                        return "\u2713"
                                                    if (statusCell.statusKey === "failed")
                                                        return "\u26A0"
                                                    return "\u2013"
                                                }
                                                color: {
                                                    if (statusCell.statusKey === "success")
                                                        return Theme.colorGreen
                                                    if (statusCell.statusKey === "failed")
                                                        return Theme.colorAccentRed
                                                    return Theme.colorTextDim
                                                }
                                                font.pixelSize: statusCell.statusKey === "failed" ? 11 : 12
                                                font.bold: true
                                                font.family: Theme.fontFamily
                                            }

                                            // Dual circular arrows (sync) — rotates while running
                                            Canvas {
                                                id: syncCanvas
                                                anchors.centerIn: parent
                                                width: 16
                                                height: 16
                                                visible: statusCell.statusKey === "running"
                                                antialiasing: true
                                                onVisibleChanged: if (visible)
                                                    requestPaint()
                                                Component.onCompleted: requestPaint()
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    ctx.clearRect(0, 0, width, height)
                                                    var ink = Theme.colorAccentBlue
                                                    ctx.strokeStyle = ink
                                                    ctx.fillStyle = ink
                                                    ctx.lineWidth = 1.6
                                                    ctx.lineCap = "round"
                                                    ctx.lineJoin = "round"

                                                    var cx = width / 2
                                                    var cy = height / 2
                                                    var r = Math.min(width, height) / 2 - 2.2

                                                    function drawArcArrow(startAng, endAng) {
                                                        // Arc body
                                                        ctx.beginPath()
                                                        ctx.arc(cx, cy, r, startAng, endAng, false)
                                                        ctx.stroke()
                                                        // Filled arrowhead at end, tangent-aligned
                                                        var tipX = cx + Math.cos(endAng) * r
                                                        var tipY = cy + Math.sin(endAng) * r
                                                        var tang = endAng + Math.PI / 2
                                                        var hx = Math.cos(tang)
                                                        var hy = Math.sin(tang)
                                                        var nx = Math.cos(endAng)
                                                        var ny = Math.sin(endAng)
                                                        var len = 3.2
                                                        var wing = 2.2
                                                        ctx.beginPath()
                                                        ctx.moveTo(tipX + hx * len, tipY + hy * len)
                                                        ctx.lineTo(tipX - nx * wing - hx * 0.4,
                                                                   tipY - ny * wing - hy * 0.4)
                                                        ctx.lineTo(tipX + nx * wing - hx * 0.4,
                                                                   tipY + ny * wing - hy * 0.4)
                                                        ctx.closePath()
                                                        ctx.fill()
                                                    }

                                                    // Two opposing arcs with arrowheads (classic sync glyph)
                                                    drawArcArrow(-Math.PI * 0.85, -Math.PI * 0.15)
                                                    drawArcArrow(Math.PI * 0.15, Math.PI * 0.85)
                                                }
                                            }

                                            NumberAnimation on spinAngle {
                                                running: statusCell.statusKey === "running"
                                                from: 0
                                                to: 360
                                                loops: Animation.Infinite
                                                duration: 1200
                                            }
                                        }

                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            visible: statusCell.statusKey === "running"
                                            text: statusCell.progressPct + "%"
                                            color: Theme.colorAccentBlue
                                            font.pixelSize: 12
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                        }

                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            visible: statusCell.statusKey === "none"
                                            text: qsTrId("aegra.common.not_available")
                                            color: Theme.colorTextDim
                                            font.pixelSize: 12
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                        }

                                        // Effective type + downgrade (Service-projected only)
                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            visible: statusCell.statusKey !== "none"
                                                     && statusCell.backupStatus.effectiveBackupTypeText
                                                     && statusCell.backupStatus.effectiveBackupTypeText.length > 0
                                            text: {
                                                var eff = statusCell.backupStatus.effectiveBackupTypeText || ""
                                                var req = statusCell.backupStatus.requestedBackupTypeText || ""
                                                if (statusCell.backupStatus.hasDowngrade && req.length > 0
                                                        && req !== eff)
                                                    return req + " → " + eff
                                                return eff
                                            }
                                            color: statusCell.backupStatus.hasDowngrade
                                                   ? Theme.colorAccentRed
                                                   : Theme.colorTextDim
                                            font.pixelSize: 11
                                            font.family: Theme.fontFamily
                                        }
                                    }

                                    Accessible.name: {
                                        var st = statusCell.backupStatus
                                        if (st.statusKey === "running")
                                            return (st.stateText || "") + " " + st.progressPercent + "%"
                                        if (st.stateText && st.stateText.length > 0)
                                            return st.stateText
                                        return qsTrId("aegra.common.not_available")
                                    }
                                }

                                // Next run — emphasized like PTS
                                Text {
                                    Layout.preferredWidth: 110
                                    text: root.timeOrNa(modelData.nextRun)
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 14
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                }

                                // Enabled toggle (compact pill)
                                Item {
                                    Layout.preferredWidth: 56
                                    Layout.fillHeight: true
                                    Rectangle {
                                        width: 36
                                        height: 20
                                        radius: 10
                                        anchors.centerIn: parent
                                        color: modelData.enabled
                                               ? Theme.colorAccentBlue : Theme.colorProgressTrack
                                        Rectangle {
                                            width: 14
                                            height: 14
                                            radius: 7
                                            anchors.verticalCenter: parent.verticalCenter
                                            x: modelData.enabled ? parent.width - width - 3 : 3
                                            color: "#ffffff"
                                            Behavior on x {
                                                NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                                            }
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.toggleScheduleEnabled(modelData.id)
                                        }
                                    }
                                }

                                // More actions
                                Item {
                                    Layout.preferredWidth: 36
                                    Layout.fillHeight: true
                                    Rectangle {
                                        id: moreBtn
                                        width: 28
                                        height: 28
                                        radius: 8
                                        anchors.centerIn: parent
                                        color: (moreHover.containsMouse || scheduleMenu.visible)
                                               ? Theme.colorHover : "transparent"
                                        //% "More actions"
                                        Accessible.name: qsTrId("aegra.backup.action.more")
                                        Text {
                                            anchors.centerIn: parent
                                            text: "\u22EE"
                                            color: Theme.colorTextGrey
                                            font.pixelSize: 15
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
                                                radius: 10
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
                                                    radius: 6
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
                                                    radius: 6
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
                                                           ? Theme.colorHoverClose : "transparent"
                                                    radius: 6
                                                }
                                                contentItem: Text {
                                                    text: parent.text
                                                    color: Theme.colorTextWhite
                                                    font.pixelSize: 12
                                                    font.family: Theme.fontFamily
                                                    verticalAlignment: Text.AlignVCenter
                                                    elide: Text.ElideRight
                                                }
                                                onTriggered: root.requestDeleteSchedule(modelData.id)
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

    // ==================== WIZARD (Add Schedule) ====================
    Item {
        id: wizardDrawer
        anchors.fill: parent
        z: 5000
        // Match main window corner radius so the dim scrim does not paint square
        // into the transparent shell corners.
        readonly property real cornerRadius: Theme.radiusWindow

        // Rounded scrim — only close when clicking outside the panel.
        Rectangle {
            id: wizardScrim
            anchors.fill: parent
            radius: wizardDrawer.cornerRadius
            color: Theme.colorScrim
            opacity: root.wizardOpen ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 250 } }
            MouseArea {
                anchors.fill: parent
                enabled: root.wizardOpen
                onClicked: root.closeWizard()
            }
        }

        Rectangle {
            id: wizardPanel
            // Vertical inset only: clear main caption above, gap at bottom.
            // Flush to the right edge (no right margin).
            readonly property int bottomInset: 0
            readonly property int topInset: 48
            width: Math.max(520, Math.min(parent.width * 0.92, parent.width))
            height: parent.height - topInset - bottomInset
            y: topInset
            // Off-canvas when closed; slide in flush to the right when open.
            x: root.wizardOpen ? (parent.width - width) : parent.width
            color: Theme.colorBg
            radius: 16
            border.width: 1
            border.color: Theme.colorBorder
            clip: true
            // Avoid intercepting clicks while fully off-screen.
            enabled: root.wizardOpen
            Behavior on x {
                NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
            }

            // Swallow clicks on empty panel areas so they do not fall through
            // to the scrim (which would close the wizard).
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onPressed: function(mouse) { mouse.accepted = true }
                onClicked: function(mouse) { mouse.accepted = true }
            }

            // Close pinned to panel top-right (aligned with step dots, not header center)
            Rectangle {
                id: wizardCloseBtn
                z: 20
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 14
                anchors.rightMargin: 14
                width: 32
                height: 32
                radius: 8
                color: closeWizardMouse.containsMouse ? Theme.colorHoverClose
                                                     : "transparent"
                Text {
                    anchors.centerIn: parent
                    text: "\u2715"
                    font.pixelSize: 14
                    color: closeWizardMouse.containsMouse ? "#ffffff"
                                                          : Theme.colorTextGrey
                }
                MouseArea {
                    id: closeWizardMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.closeWizard()
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                // Leave room for the absolute close control on the right.
                anchors.rightMargin: 52
                spacing: 16

                // Header: back + step progress bar
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 64
                    spacing: 8

                    // Back — keep slot width stable so the step bar does not jump
                    Item {
                        Layout.preferredWidth: 36
                        Layout.preferredHeight: 36
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 0

                        Rectangle {
                            anchors.top: parent.top
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 32
                            height: 32
                            radius: 16
                            opacity: root.wizardStep > 0 ? 1 : 0
                            enabled: root.wizardStep > 0
                            color: backMouse.containsMouse ? Theme.colorHover : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: "\u25C0"
                                font.pixelSize: 11
                                color: Theme.colorTextGrey
                            }
                            MouseArea {
                                id: backMouse
                                anchors.fill: parent
                                enabled: root.wizardStep > 0
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (root.wizardStep > 0)
                                        root.wizardStep--
                                }
                            }
                        }
                    }

                    // Step progress (replaces plain wizard title)
                    Item {
                        id: stepBar
                        Layout.fillWidth: true
                        Layout.preferredHeight: 56
                        Layout.alignment: Qt.AlignVCenter

                        readonly property int stepCount: 3
                        readonly property real slotW: width / stepCount
                        readonly property real lineY: 14
                        readonly property real lineLeft: slotW * 0.5
                        readonly property real lineSpan: slotW * (stepCount - 1)

                        // Track
                        Rectangle {
                            x: stepBar.lineLeft
                            y: stepBar.lineY
                            width: stepBar.lineSpan
                            height: 3
                            radius: 1.5
                            color: Theme.colorProgressTrack
                        }
                        // Progress fill between step centers
                        Rectangle {
                            x: stepBar.lineLeft
                            y: stepBar.lineY
                            width: stepBar.lineSpan
                                 * (root.wizardStep / Math.max(1, stepBar.stepCount - 1))
                            height: 3
                            radius: 1.5
                            color: Theme.colorAccentBlue
                            Behavior on width {
                                NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
                            }
                        }

                        Repeater {
                            model: stepBar.stepCount
                            delegate: Item {
                                width: stepBar.slotW
                                height: stepBar.height
                                x: index * stepBar.slotW
                                y: 0

                                readonly property bool done: index < root.wizardStep
                                readonly property bool current: index === root.wizardStep

                                Rectangle {
                                    id: stepDot
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: 2
                                    width: 26
                                    height: 26
                                    radius: 13
                                    border.width: (parent.done || parent.current) ? 0 : 2
                                    border.color: Theme.colorBorder
                                    color: (parent.done || parent.current)
                                           ? Theme.colorAccentBlue
                                           : Theme.colorCard
                                    Behavior on color {
                                        ColorAnimation { duration: 200 }
                                    }

                                    Text {
                                        anchors.centerIn: parent
                                        text: parent.parent.done
                                              ? "\u2713"
                                              : ("" + (index + 1))
                                        color: (parent.parent.done || parent.parent.current)
                                               ? "#ffffff"
                                               : Theme.colorTextDim
                                        font.pixelSize: parent.parent.done ? 12 : 11
                                        font.bold: true
                                        font.family: Theme.fontFamily
                                    }
                                }

                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.top: stepDot.bottom
                                    anchors.topMargin: 6
                                    width: parent.width - 4
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                    text: root.wizardStepLabels[index] || ""
                                    color: parent.current ? Theme.colorTextWhite
                                           : (parent.done ? Theme.colorAccentBlue
                                                          : Theme.colorTextDim)
                                    font.pixelSize: 11
                                    font.bold: parent.current
                                    font.family: Theme.fontFamily
                                }
                            }
                        }
                    }
                }

                Item {
                    id: wizardStepContainer
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    // -------- Step 0: TYPE SELECTION CARDS (Disk/Volume vs Files) --------
                    Item {
                        id: wizardStep0
                        anchors.fill: parent
                        visible: opacity > 0.001
                        opacity: root.wizardStep === 0 ? 1 : 0
                        transform: Translate {
                            x: root.wizardStep === 0 ? 0 : (root.wizardStep > 0 ? -wizardStepContainer.width : wizardStepContainer.width)
                        }
                        Behavior on opacity { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
                        Behavior on transform { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }

                        ColumnLayout {
                            anchors.fill: parent
                        spacing: 24
                        Layout.alignment: Qt.AlignTop

                        Item { height: 12 }

                        ColumnLayout {
                            spacing: 6
                            Layout.alignment: Qt.AlignHCenter

                            Text {
                                //% "Choose backup protection type"
                                text: qsTrId("aegra.backup.wizard.type_title")
                                color: Theme.colorTextWhite
                                font.pixelSize: 22
                                font.bold: true
                                font.family: Theme.fontFamily
                                Layout.alignment: Qt.AlignHCenter
                            }
                            Text {
                                //% "Select block-level protection for full disks/volumes, or versioned protection for specific files and folders"
                                text: qsTrId("aegra.backup.wizard.type_subtitle")
                                color: Theme.colorTextGrey
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                Layout.alignment: Qt.AlignHCenter
                                wrapMode: Text.WordWrap
                                horizontalAlignment: Text.AlignHCenter
                                Layout.fillWidth: true
                            }
                        }

                        Item { height: 8 }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 24
                            Layout.alignment: Qt.AlignHCenter

                            // Card 1: Disk / Volume 备份
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 320
                                implicitHeight: 300
                                radius: 20
                                color: diskCardMouse.containsMouse ? Theme.colorHover : Theme.colorCard
                                border.width: 1
                                border.color: diskCardMouse.containsMouse ? Theme.colorAccentBlue : Theme.colorBorder
                                Behavior on color { ColorAnimation { duration: 150 } }

                                MouseArea {
                                    id: diskCardMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        root.backupMode = "disk"
                                        root.wizardStep = 1
                                    }
                                }

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 24
                                    spacing: 16

                                    Rectangle {
                                        width: 56
                                        height: 56
                                        radius: 18
                                        Layout.alignment: Qt.AlignHCenter
                                        gradient: Gradient {
                                            GradientStop { position: 0.0; color: "#3B82F6" }
                                            GradientStop { position: 1.0; color: "#2563EB" }
                                        }
                                        DiskIcon { anchors.centerIn: parent; size: 34; variant: "system" }
                                    }

                                    Text {
                                        //% "Disk / Volume backup"
                                        text: qsTrId("aegra.backup.wizard.mode.disk_title")
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 18
                                        font.bold: true
                                        font.family: Theme.fontFamily
                                        Layout.alignment: Qt.AlignHCenter
                                    }

                                    Text {
                                        //% "Full and incremental block-level backup of Windows system volumes, physical disks, and logical volumes (supports disaster recovery and full-disk restore)."
                                        text: qsTrId("aegra.backup.wizard.mode.disk_desc")
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                        horizontalAlignment: Text.AlignHCenter
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                    }

                                    Rectangle {
                                        height: 38
                                        Layout.fillWidth: true
                                        radius: 19
                                        color: Theme.colorAccentBlue
                                        Text {
                                            anchors.centerIn: parent
                                            //% "Choose Disk / Volume backup →"
                                            text: qsTrId("aegra.backup.wizard.mode.disk_action")
                                            color: "#ffffff"
                                            font.pixelSize: 13
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                        }
                                    }
                                }
                            }

                            // Card 2: Files 备份
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredWidth: 320
                                implicitHeight: 300
                                radius: 20
                                color: filesCardMouse.containsMouse ? Theme.colorHover : Theme.colorCard
                                border.width: 1
                                border.color: filesCardMouse.containsMouse ? Theme.colorGreen : Theme.colorBorder
                                Behavior on color { ColorAnimation { duration: 150 } }

                                MouseArea {
                                    id: filesCardMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: serviceClient.fileBrowseAvailable === true
                                    cursorShape: enabled ? Qt.PointingHandCursor
                                                         : Qt.ForbiddenCursor
                                    onClicked: {
                                        root.backupMode = "files"
                                        root.wizardStep = 1
                                        if (serviceClient.fileBrowseAvailable)
                                            serviceClient.loadFileBrowseRoots()
                                    }
                                }
                                // Dim when Service lacks file.browse.
                                Rectangle {
                                    anchors.fill: parent
                                    radius: 20
                                    color: "#80000000"
                                    visible: serviceClient.fileBrowseAvailable !== true
                                    z: 5
                                    Text {
                                        anchors.centerIn: parent
                                        width: parent.width - 32
                                        horizontalAlignment: Text.AlignHCenter
                                        wrapMode: Text.WordWrap
                                        //% "File browse is not available on this Service"
                                        text: qsTrId("aegra.backup.wizard.files_unavailable")
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                    }
                                }

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 24
                                    spacing: 16

                                    Rectangle {
                                        width: 56
                                        height: 56
                                        radius: 18
                                        Layout.alignment: Qt.AlignHCenter
                                        gradient: Gradient {
                                            GradientStop { position: 0.0; color: "#10B981" }
                                            GradientStop { position: 1.0; color: "#059669" }
                                        }
                                        Text { anchors.centerIn: parent; text: "📁"; font.pixelSize: 26 }
                                    }

                                    Text {
                                        //% "Files backup"
                                        text: qsTrId("aegra.backup.wizard.mode.files_title")
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 18
                                        font.bold: true
                                        font.family: Theme.fontFamily
                                        Layout.alignment: Qt.AlignHCenter
                                    }

                                    Text {
                                        //% "Select documents, project folders, or data directories for continuous versioned protection and lightweight folder sync."
                                        text: qsTrId("aegra.backup.wizard.mode.files_desc")
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                        horizontalAlignment: Text.AlignHCenter
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                    }

                                    Rectangle {
                                        height: 38
                                        Layout.fillWidth: true
                                        radius: 19
                                        color: Theme.colorGreen
                                        Text {
                                            anchors.centerIn: parent
                                            //% "Choose Files backup →"
                                            text: qsTrId("aegra.backup.wizard.mode.files_action")
                                            color: "#ffffff"
                                            font.pixelSize: 13
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                        }
                                    }
                                }
                            }
                        }
                        } // step0 ColumnLayout
                    } // wizardStep0

                    // -------- Step 1: SOURCE | DESTINATION --------
                    Item {
                        id: wizardStep1
                        anchors.fill: parent
                        visible: opacity > 0.001
                        opacity: root.wizardStep === 1 ? 1 : 0
                        transform: Translate {
                            x: root.wizardStep === 1 ? 0 : (root.wizardStep < 1 ? wizardStepContainer.width : -wizardStepContainer.width)
                        }
                        Behavior on opacity { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
                        Behavior on transform { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }

                        ColumnLayout {
                            anchors.fill: parent
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

                                // Header right: refresh inventory (disks + file browse)
                                headerRightComponent: Component {
                                    MouseArea {
                                        id: refreshBtnArea
                                        width: 28
                                        height: 28
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            spinAnim.restart()
                                            serviceClient.refreshInventory()
                                            if (serviceClient.fileBrowseAvailable) serviceClient.loadFileBrowseRoots()
                                        }

                                        Rectangle {
                                            anchors.fill: parent
                                            radius: 6
                                            color: refreshBtnArea.pressed ? Theme.colorButtonHover : (refreshBtnArea.containsMouse ? Theme.colorHover : "transparent")
                                            border.width: 0

                                            NavIcon {
                                                id: refreshNavIcon
                                                anchors.centerIn: parent
                                                width: 16
                                                height: 16
                                                name: "refresh"
                                                color: refreshBtnArea.containsMouse ? Theme.colorAccentBlue : Theme.colorTextGrey

                                                NumberAnimation on rotation {
                                                    id: spinAnim
                                                    running: false
                                                    from: 0
                                                    to: 360
                                                    duration: 400
                                                    easing.type: Easing.InOutQuad
                                                }
                                            }
                                        }
                                    }
                                }

                                // File-set selection summary — hidden per UI request.
                                Text {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.topMargin: 50
                                    anchors.leftMargin: 16
                                    anchors.rightMargin: 16
                                    visible: false
                                    //% "Selected: %1"
                                    text: qsTrId("aegra.file.browse.selected_label").arg(
                                              serviceClient.fileBrowseSources
                                              ? serviceClient.fileBrowseSources.selectionSummary
                                              : "")
                                    color: Theme.colorAccentBlue
                                    font.pixelSize: 11
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideMiddle
                                    z: 2
                                }

                                ListView {
                                    id: fileSourceList
                                    anchors.fill: parent
                                    anchors.topMargin: 50
                                    anchors.margins: 16
                                    clip: true
                                    spacing: 2
                                    visible: root.backupMode === "files"
                                    model: serviceClient.fileBrowseSources
                                    delegate: Rectangle {
                                        required property int index
                                        required property string nodeToken
                                        required property string displayName
                                        required property bool hasChildren
                                        required property bool isDirectory
                                        required property int depth
                                        required property bool expanded
                                        required property bool nodeLoading
                                        required property int checkState
                                        required property bool isSelectable
                                        required property string disabledReason
                                        width: fileSourceList.width
                                        height: 36
                                        radius: 4
                                        color: fileRowHover.containsMouse
                                               ? Theme.colorHover : Theme.colorListItem
                                        opacity: isSelectable ? 1.0 : 0.55
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 8 + Math.max(0, depth) * 14
                                            anchors.rightMargin: 8
                                            spacing: 8
                                            Text {
                                                text: hasChildren
                                                      ? (expanded ? "▾" : "▸")
                                                      : " "
                                                color: Theme.colorTextGrey
                                                font.pixelSize: 12
                                                Layout.preferredWidth: 14
                                                MouseArea {
                                                    anchors.fill: parent
                                                    enabled: hasChildren
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: serviceClient.fileBrowseSources
                                                                   .toggleExpanded(nodeToken)
                                                }
                                            }
                                            Rectangle {
                                                width: 16
                                                height: 16
                                                radius: 3
                                                visible: isSelectable
                                                color: checkState === 2
                                                       ? Theme.colorAccentBlue
                                                       : (checkState === 1
                                                          ? Theme.colorAccentBlue
                                                          : "transparent")
                                                opacity: checkState === 1 ? 0.45 : 1.0
                                                border.width: 2
                                                border.color: checkState > 0
                                                              ? Theme.colorAccentBlue
                                                              : Theme.colorTextGrey
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: checkState === 2 ? "\u2713"
                                                          : (checkState === 1 ? "−" : "")
                                                    color: "white"
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: serviceClient.fileBrowseSources
                                                                   .toggleChecked(nodeToken)
                                                }
                                            }
                                            // Icon before name: special folder / volume / folder / file.
                                            Loader {
                                                Layout.preferredWidth: 16
                                                Layout.preferredHeight: 16
                                                Layout.alignment: Qt.AlignVCenter
                                                sourceComponent: root.fileSourceIconFor(
                                                    depth, isDirectory, displayName)
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: displayName
                                                      + (nodeLoading ? " …" : "")
                                                      + (disabledReason
                                                         ? ("  (" + disabledReason + ")")
                                                         : "")
                                                color: Theme.colorTextWhite
                                                font.pixelSize: 12
                                                font.family: Theme.fontFamily
                                                elide: Text.ElideMiddle
                                            }
                                        }
                                        MouseArea {
                                            id: fileRowHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            acceptedButtons: Qt.NoButton
                                            z: -1
                                        }
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        width: parent.width - 24
                                        horizontalAlignment: Text.AlignHCenter
                                        wrapMode: Text.WordWrap
                                        visible: fileSourceList.count === 0
                                        text: {
                                            if (serviceClient.fileBrowseSources
                                                    && serviceClient.fileBrowseSources.loading)
                                                //% "Loading folders..."
                                                return qsTrId("aegra.file.browse.loading")
                                            if (serviceClient.fileBrowseSources
                                                    && serviceClient.fileBrowseSources.errorText)
                                                return serviceClient.fileBrowseSources.errorText
                                            //% "Expand a drive or folder to select files"
                                            return qsTrId("aegra.file.browse.empty")
                                        }
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                    }
                                }

                                ScrollView {
                                    id: sourceScroll
                                    anchors.fill: parent
                                    anchors.topMargin: 50
                                    anchors.margins: 16
                                    clip: true
                                    contentWidth: availableWidth
                                    visible: root.backupMode !== "files"

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

                        // Footer: Back + Next (wizard step 1)
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 15
                            Item { Layout.fillWidth: true }

                            AppButton {
                                //% "Back"
                                text: qsTrId("aegra.common.back")
                                Layout.preferredWidth: 100
                                Layout.preferredHeight: 40
                                onClicked: root.wizardStep = 0
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
                        } // step1 ColumnLayout
                    } // wizardStep1

                    // -------- Step 2: Schedule settings + Options --------
                    Item {
                        id: wizardStep2Item
                        anchors.fill: parent
                        visible: opacity > 0.001
                        opacity: root.wizardStep === 2 ? 1 : 0
                        transform: Translate {
                            x: root.wizardStep === 2 ? 0 : (root.wizardStep < 2 ? wizardStepContainer.width : -wizardStepContainer.width)
                        }
                        Behavior on opacity { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
                        Behavior on transform { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }

                        BackupWizardStep2 {
                            id: wizardStep2
                            anchors.fill: parent
                            filesMode: root.backupMode === "files"
                            onBackRequested: root.wizardStep = 1
                            onCreateRequested: root.createScheduleFromWizard()
                        }
                    } // wizardStep2Item
                } // wizardStepContainer
            } // ColumnLayout (wizard body)
        } // wizardPanel
    } // wizardDrawer
} // root

