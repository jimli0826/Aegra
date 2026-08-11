import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Visual baseline: backup/src/gui RestorePage — Source|Target left, Options right (~30%).
Item {
    id: root
    //% "Restore"
    Accessible.name: qsTrId("aegra.nav.restore")
    /// Request Main to switch to Home after a restore job is accepted.
    signal navigateHomeRequested()

    Component {
        id: restoreVolumeIconComponent
        DiskIcon { size: 16; variant: "hdd" }
    }
    Component {
        id: restoreFolderIconComponent
        FolderIcon { size: 16 }
    }
    Component {
        id: restoreFileIconComponent
        FileDocIcon { size: 16 }
    }
    Component {
        id: restoreSpecialDesktopIcon
        SpecialFolderIcon { size: 16; variant: "desktop" }
    }
    Component {
        id: restoreSpecialDownloadsIcon
        SpecialFolderIcon { size: 16; variant: "downloads" }
    }
    Component {
        id: restoreSpecialDocumentsIcon
        SpecialFolderIcon { size: 16; variant: "documents" }
    }
    Component {
        id: restoreSpecialPicturesIcon
        SpecialFolderIcon { size: 16; variant: "pictures" }
    }
    Component {
        id: restoreSpecialMusicIcon
        SpecialFolderIcon { size: 16; variant: "music" }
    }
    Component {
        id: restoreSpecialVideosIcon
        SpecialFolderIcon { size: 16; variant: "videos" }
    }

    function isVolumeRootLabel(name) {
        var text = String(name || "")
        return /\([A-Za-z]:\)$/.test(text) || /^[A-Za-z]:$/.test(text)
    }
    function restoreTargetIconFor(depth, isDirectory, displayName) {
        if (depth === 0) {
            switch (String(displayName || "").toLowerCase()) {
            case "desktop": return restoreSpecialDesktopIcon
            case "downloads": return restoreSpecialDownloadsIcon
            case "documents": return restoreSpecialDocumentsIcon
            case "pictures": return restoreSpecialPicturesIcon
            case "music": return restoreSpecialMusicIcon
            case "videos": return restoreSpecialVideosIcon
            default:
                return isVolumeRootLabel(displayName)
                       ? restoreVolumeIconComponent
                       : restoreFolderIconComponent
            }
        }
        if (isDirectory)
            return restoreFolderIconComponent
        return restoreFileIconComponent
    }

    /// Options expanded by default (old RestorePage: optionsCollapsed: false).
    property bool optionsCollapsed: false
    readonly property int optionsCollapsedWidth: 36
    property bool checkpointPanelOpen: false
    property bool preserveSignature: true
    property bool autoExtend: true
    /// Vertical split Source / Target (0.2 … 0.8)
    property real sourceTargetRatio: 0.45
    /// Options width share when expanded (0.18 … 0.45)
    property real optionsPaneRatio: 0.30
    property string selectedCheckpointId: ""
    property string selectedCheckpointLabel: ""
    property string panelSelectedDate: ""
    property var panelCheckpoints: []
    property int panelCheckpointsEpoch: 0
    /// 0 = restore type; 1 = source & destination workspace; 2 = summary + progress.
    property int restoreStep: 0
    /// "disk" | "volume" | "files" — fixed after type card selection.
    property string restoreMode: "disk"
    readonly property bool isVolumeMode: root.restoreMode === "volume"
    readonly property bool isFileMode: root.restoreMode === "files"
    readonly property bool onTypeStep: root.restoreStep === 0
    readonly property bool onWorkspaceStep: root.restoreStep === 1
    readonly property bool onSummaryStep: root.restoreStep === 2
    /// Step labels for the progress bar (Source & destination + Summary only; type selection has stat cards instead).
    readonly property var restoreStepLabels: [
        qsTrId("aegra.restore.wizard.step.source_dest"),
        qsTrId("aegra.restore.wizard.step.summary")
    ]
    /// Session tracking for summary progress (ms since epoch; 0 = inactive).
    property double restoreSessionStartMs: 0
    property bool restoreJobsSubmitted: false
    /// True when the last start attempt failed (allows Back with no job rows).
    property bool restoreSessionFailed: false
    /// Localized error for the summary progress card (start failure or job failure).
    property string restoreSessionErrorText: ""
    /// True while workspace Next is waiting on file-restore preflight (capacity check).
    property bool filePreflightPending: false
    /// 1 volume_set, 2 file_set — taken from selected checkpoint.
    property int selectedContentKind: 1
    /// File restore conflict policy: 1 fail, 2 replace, 3 rename.
    property int fileConflictPolicy: 1
    /// File restore: apply Owner/Group/DACL/SACL from archive (default on).
    property bool fileRestoreSecurity: true

    /// Source disks from Service GetRecoveryPointLayout (Manifest volumes). Empty until a
    /// checkpoint is selected and the layout query succeeds.
    readonly property var sourceDisks: serviceClient.recoveryPointSourceDisks
    readonly property var sourceVolumes: serviceClient.recoveryPointSourceVolumes
    readonly property bool sourceLayoutLoading: serviceClient.recoveryPointLayoutLoading
    readonly property string sourceLayoutError: serviceClient.recoveryPointLayoutErrorText

    /// Live inventory disks (same tree as Backup), else empty list.
    readonly property var targetDisks: {
        if (serviceClient.sources && serviceClient.sources.count > 0
                && serviceClient.sources.disksTree
                && serviceClient.sources.disksTree.length > 0)
            return serviceClient.sources.disksTree
        return []
    }

    /// Flat inventory volumes (vol.*) for volume-mode targets.
    readonly property var targetVolumes: {
        var _dep = serviceClient.sources ? serviceClient.sources.count : 0
        var out = []
        var disks = root.targetDisks || []
        for (var i = 0; i < disks.length; ++i) {
            var vols = disks[i].volumes || []
            for (var j = 0; j < vols.length; ++j) {
                var v = vols[j]
                if (!v || !v.sourceId)
                    continue
                out.push(v)
            }
        }
        return out
    }

    /// Source disk_number (string key) → target disk_number, or -1 when unmapped.
    property var diskMappings: ({})
    /// Source volumeIndex (string key) → target sourceId, or "" when unmapped.
    property var volumeMappings: ({})
    /// Bumped on every mapping change so ComboBox/model bindings refresh.
    property int mappingEpoch: 0
    /// True while a source-disk drag hovers a target that fails restore checks.
    property bool mappingDropBlocked: false
    /// Localized block reason shown after the ban icon on the drag ghost.
    property string mappingDropBlockReason: ""

    function setMappingDropFeedback(blocked, reason) {
        root.mappingDropBlocked = blocked
        root.mappingDropBlockReason = blocked ? (reason || "") : ""
    }

    function clearMappingDropBlocked() {
        root.mappingDropBlocked = false
        root.mappingDropBlockReason = ""
    }

    function resetFileRestoreOptions() {
        root.fileConflictPolicy = 1
        root.fileRestoreSecurity = true
    }

    function selectRestoreType(mode) {
        if (mode !== "disk" && mode !== "volume" && mode !== "files")
            return
        if (mode === "files" && serviceClient.fileRestoreAvailable !== true)
            return
        root.restoreMode = mode
        root.restoreStep = 1
        root.restoreSessionStartMs = 0
        root.restoreJobsSubmitted = false
        root.restoreSessionFailed = false
        root.restoreSessionErrorText = ""
        root.resetFileRestoreOptions()
        root.applySelectedCheckpoint(null)
        root.checkpointPanelOpen = false
        root.optionsCollapsed = false
        // Target folder is local PC inventory — show immediately, no checkpoint needed.
        if (mode === "files" && typeof serviceClient.loadFileRestoreTargetRoots === "function")
            serviceClient.loadFileRestoreTargetRoots()
    }

    function clearRestoreSession() {
        root.restoreSessionStartMs = 0
        root.restoreJobsSubmitted = false
        root.restoreSessionFailed = false
        root.restoreSessionErrorText = ""
        root.pendingRestoreQueue = []
        root.multiRestoreActive = false
        root.filePreflightPending = false
    }

    function goBackToTypeSelection() {
        root.clearRestoreSession()
        root.restoreStep = 0
        root.checkpointPanelOpen = false
        root.resetFileRestoreOptions()
        root.applySelectedCheckpoint(null)
    }

    /// Active restore job anchor (created_utc_ms), or 0 when none are running.
    function activeRestoreAnchorMs() {
        if (!serviceClient.jobs
                || typeof serviceClient.jobs.earliestActiveRestoreCreatedUtcMs !== "function")
            return 0
        return Number(serviceClient.jobs.earliestActiveRestoreCreatedUtcMs()) || 0
    }

    /// Reattach Summary while a restore job is active; otherwise return to step 0 on page entry.
    /// forceIdleToTypeStep: when true (page became visible), idle sessions reset to type selection.
    function reconcileRestoreEntry(forceIdleToTypeStep) {
        var anchor = root.activeRestoreAnchorMs()
        if (anchor > 0) {
            // Prefer the earlier of local session start and Service job creation.
            if (root.restoreSessionStartMs <= 0 || root.restoreSessionStartMs > anchor)
                root.restoreSessionStartMs = anchor
            root.restoreJobsSubmitted = true
            root.restoreSessionFailed = false
            root.restoreSessionErrorText = ""
            root.multiRestoreActive = false
            root.filePreflightPending = false
            root.checkpointPanelOpen = false
            if (root.restoreStep !== 2)
                root.restoreStep = 2
            return
        }
        if (!forceIdleToTypeStep)
            return
        // No running restore: landing on Restore shows the first step (type selection).
        // Keep an in-page terminal summary only until the user leaves the page.
        if (root.restoreStep === 0 && !root.restoreJobsSubmitted
                && root.restoreSessionStartMs <= 0)
            return
        root.goBackToTypeSelection()
    }

    /// True while a restore start is in-flight or jobs from this session are still active.
    /// Preflight-only (Next) busy must not block the step-bar Back button.
    readonly property bool restoreSessionRunning: {
        // Depend on job list revisions so reattach/active anchors rebind.
        var _rev = serviceClient.jobs ? serviceClient.jobs.revision : 0
        var _active = serviceClient.jobs ? serviceClient.jobs.activeCount : 0
        if (root.multiRestoreActive)
            return true
        if (serviceClient.restoreCommandBusy && !root.filePreflightPending
                && root.restoreSessionStarted)
            return true
        // Service-side active restore (covers reattach after page navigation).
        if (root.activeRestoreAnchorMs() > 0)
            return true
        if (!root.restoreJobsSubmitted || root.restoreSessionFailed)
            return false
        var st = root.restoreSessionStatus
        return st && st.jobCount > 0 && st.allTerminal !== true
    }

    /// One step back (Backup-aligned). Blocked while restore runs or after success.
    function stepBarBack() {
        if (root.restoreStep <= 0)
            return
        if (root.restoreSessionRunning || root.restoreProgressSucceeded)
            return
        if (root.restoreStep === 2) {
            root.clearRestoreSession()
            root.restoreStep = 1
            return
        }
        // step 1 → type selection (step 0)
        root.goBackToTypeSelection()
    }

    function goToSummary() {
        if (!root.canRestore || root.filePreflightPending)
            return
        if (root.isFileMode) {
            // Capacity / eligibility check on Next (not on Restore start).
            root.filePreflightPending = true
            if (!serviceClient.prepareFileRestore(root.selectedCheckpointId,
                                                  root.fileConflictPolicy,
                                                  root.pendingLayoutPassword,
                                                  root.fileRestoreSecurity)) {
                root.filePreflightPending = false
            }
            return
        }
        root.clearRestoreSession()
        root.restoreStep = 2
    }

    /// Normalize inventory mount letter to "X:" (accepts "X", "X:", "X:\\").
    function normalizeDriveLetter(raw) {
        var s = (raw || "").toString().replace(/[\\/]/g, "").trim().toUpperCase()
        if (s.length === 0)
            return ""
        var ch = s.charAt(0)
        if (ch < "A" || ch > "Z")
            return ""
        return ch + ":"
    }

    /// Extract drive letter from browse root labels such as "新加卷 (Z:)" or "System (C:)".
    function extractDriveLetterFromLabel(displayName) {
        var dn = (displayName || "").toString()
        var m = dn.match(/\(([A-Za-z]):\)/)
        if (m && m[1])
            return m[1].toUpperCase() + ":"
        m = dn.match(/^([A-Za-z]):\s*$/)
        if (m && m[1])
            return m[1].toUpperCase() + ":"
        return ""
    }

    /// Inventory volume for a file-browse root, matched by drive letter only.
    function volumeInventoryForLabel(displayName) {
        var want = root.extractDriveLetterFromLabel(displayName)
        if (want.length === 0)
            return null
        var vols = root.targetVolumes || []
        for (var i = 0; i < vols.length; ++i) {
            var v = vols[i]
            if (!v)
                continue
            if (root.normalizeDriveLetter(v.letter) === want)
                return v
        }
        return null
    }

    /// Format: "7.03 GB free, 29.90 GB total" / "7.03 GB 可用, 共 29.90 GB".
    function volumeFreeTotalText(displayName) {
        var v = root.volumeInventoryForLabel(displayName)
        if (!v)
            return ""
        var freeText = (v.free || "").toString()
        var totalText = (v.size || "").toString()
        if (freeText.length > 0 && totalText.length > 0)
            //% "%1 free, %2 total"
            return qsTrId("aegra.restore.file.volume_free_total").arg(freeText).arg(totalText)
        if (totalText.length > 0)
            return totalText
        return ""
    }

    /// Used fraction 0..1 for volume-root background fill (capacity − free).
    function volumeUsedRatio(displayName) {
        var v = root.volumeInventoryForLabel(displayName)
        if (!v)
            return 0
        var cap = Number(v.capacityBytes) || 0
        if (cap <= 0)
            return 0
        var free = Number(v.freeBytes)
        if (isNaN(free) || free < 0)
            free = 0
        if (free > cap)
            free = cap
        return Math.min(1, Math.max(0, (cap - free) / cap))
    }

    /// True only after the user has pressed Restore on the summary step.
    readonly property bool restoreSessionStarted: root.restoreSessionStartMs > 0
            || root.restoreJobsSubmitted

    /// Live restore progress for the current summary session (depends on jobs.revision).
    /// Before start: never query historical jobs — that painted a false progress bar.
    readonly property var restoreSessionStatus: {
        var empty = {
            jobCount: 0, activeCount: 0, progressPercent: 0, stateText: "",
            messageText: "", sourceName: "", statusKey: "none",
            allTerminal: false, anyFailed: false
        }
        if (!root.restoreSessionStarted)
            return empty
        var rev = serviceClient.jobs ? serviceClient.jobs.revision : 0
        var _busy = serviceClient.restoreCommandBusy
        var _multi = root.multiRestoreActive
        var since = root.restoreSessionStartMs > 0
                    ? Math.floor(root.restoreSessionStartMs) - 5000
                    : 0
        if (!serviceClient.jobs || typeof serviceClient.jobs.restoreSessionStatus !== "function")
            return empty
        var st = serviceClient.jobs.restoreSessionStatus(since)
        return st || empty
    }

    readonly property bool restoreSessionComplete: {
        if (!root.onSummaryStep || !root.restoreSessionStarted)
            return false
        if (serviceClient.restoreCommandBusy || root.multiRestoreActive)
            return false
        if (!root.restoreJobsSubmitted)
            return false
        var st = root.restoreSessionStatus
        // Wait for job rows to become terminal when the Service lists them.
        if (st.jobCount > 0)
            return st.allTerminal === true
        // No job rows: only finish early on an explicit start failure so Back is available.
        return root.restoreSessionFailed
    }

    readonly property int restoreProgressPercent: {
        if (!root.restoreSessionStarted)
            return 0
        var st = root.restoreSessionStatus
        if (st && st.jobCount > 0)
            return st.progressPercent || 0
        if (serviceClient.restoreCommandBusy || root.multiRestoreActive)
            return 0
        if (root.restoreSessionComplete)
            return 100
        return 0
    }

    readonly property bool restoreProgressActive: root.restoreSessionStarted
            && !root.restoreSessionComplete
            && (serviceClient.restoreCommandBusy
                || root.multiRestoreActive
                || (root.restoreSessionStatus && root.restoreSessionStatus.activeCount > 0))

    /// Failed start or terminal job failure — red progress fill.
    readonly property bool restoreProgressFailed: {
        if (root.restoreSessionFailed)
            return true
        var st = root.restoreSessionStatus
        return !!(st && st.anyFailed)
    }

    /// Session finished without failure — green progress fill.
    readonly property bool restoreProgressSucceeded: root.restoreSessionComplete
            && !root.restoreProgressFailed

    readonly property string restoreProgressErrorText: {
        if (root.restoreSessionErrorText && root.restoreSessionErrorText.length > 0)
            return root.restoreSessionErrorText
        var st = root.restoreSessionStatus
        if (st && st.anyFailed && st.messageText && st.messageText.length > 0)
            return st.messageText
        return ""
    }

    readonly property color restoreProgressFillColor: {
        if (root.restoreProgressFailed)
            return Theme.colorAccentRed
        if (root.restoreProgressSucceeded)
            return Theme.colorGreen
        if (root.restoreProgressActive)
            return Theme.colorAccentBlue
        return Theme.colorTextDim
    }

    readonly property string restoreProgressLabel: {
        if (!root.restoreSessionStarted) {
            //% "Review the selection, then start restore."
            return qsTrId("aegra.restore.summary.ready")
        }
        if (root.restoreSessionComplete) {
            if (root.restoreProgressFailed)
                //% "Restore finished with errors"
                return qsTrId("aegra.restore.summary.finished_errors")
            //% "Restore completed"
            return qsTrId("aegra.restore.summary.finished")
        }
        if (serviceClient.restoreCommandBusy || root.multiRestoreActive)
            //% "Starting restore..."
            return qsTrId("aegra.restore.summary.starting")
        var st = root.restoreSessionStatus
        if (st && st.stateText && st.stateText.length > 0) {
            var name = st.sourceName || ""
            if (name.length > 0)
                return name + " — " + st.stateText
            return st.stateText
        }
        //% "Waiting for restore progress..."
        return qsTrId("aegra.restore.summary.waiting")
    }

    readonly property string restoreModeTitle: {
        if (root.isFileMode)
            //% "Files / folders"
            return qsTrId("aegra.restore.type.files_title")
        if (root.isVolumeMode)
            //% "Volume restore"
            return qsTrId("aegra.restore.type.volume_title")
        //% "Disk restore"
        return qsTrId("aegra.restore.type.disk_title")
    }

    readonly property string fileConflictPolicyLabel: {
        if (root.fileConflictPolicy === 2)
            //% "Overwrite existing"
            return qsTrId("aegra.restore.file.conflict_replace")
        if (root.fileConflictPolicy === 3)
            //% "Rename restored file"
            return qsTrId("aegra.restore.file.conflict_rename")
        //% "Skip (fail on conflict)"
        return qsTrId("aegra.restore.file.conflict_fail")
    }

    function optionOnOff(enabled) {
        //% "On"
        //% "Off"
        return enabled ? qsTrId("aegra.common.on") : qsTrId("aegra.common.off")
    }

    ParallelAnimation {
        id: typeCardsEntranceAnim

        // Header entrance
        ParallelAnimation {
            NumberAnimation { target: restoreTypeHeader; property: "opacity"; from: 0; to: 1; duration: 360; easing.type: Easing.OutCubic }
            NumberAnimation { target: restoreTypeHeaderTrans; property: "y"; from: 18; to: 0; duration: 420; easing.type: Easing.OutCubic }
        }

        // Card 1: Disk restore
        SequentialAnimation {
            PauseAnimation { duration: 20 }
            ParallelAnimation {
                NumberAnimation { target: restoreTypeCard1; property: "opacity"; from: 0; to: 1; duration: 380; easing.type: Easing.OutCubic }
                NumberAnimation { target: restoreTypeCard1; property: "scale"; from: 0.88; to: 1.0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
                NumberAnimation { target: restoreTypeCardTrans1; property: "y"; from: 52; to: 0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
            }
        }

        // Card 2: Volume restore
        SequentialAnimation {
            PauseAnimation { duration: 110 }
            ParallelAnimation {
                NumberAnimation { target: restoreTypeCard2; property: "opacity"; from: 0; to: 1; duration: 380; easing.type: Easing.OutCubic }
                NumberAnimation { target: restoreTypeCard2; property: "scale"; from: 0.88; to: 1.0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
                NumberAnimation { target: restoreTypeCardTrans2; property: "y"; from: 52; to: 0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
            }
        }

        // Card 3: Files restore
        SequentialAnimation {
            PauseAnimation { duration: 200 }
            ParallelAnimation {
                NumberAnimation { target: restoreTypeCard3; property: "opacity"; from: 0; to: 1; duration: 380; easing.type: Easing.OutCubic }
                NumberAnimation { target: restoreTypeCard3; property: "scale"; from: 0.88; to: 1.0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
                NumberAnimation { target: restoreTypeCardTrans3; property: "y"; from: 52; to: 0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
            }
        }
    }

    function playTypeCardsEntrance() {
        typeCardsEntranceAnim.stop()
        restoreTypeHeader.opacity = 0
        restoreTypeHeaderTrans.y = 18
        restoreTypeCard1.opacity = 0
        restoreTypeCard1.scale = 0.88
        restoreTypeCardTrans1.y = 52
        restoreTypeCard2.opacity = 0
        restoreTypeCard2.scale = 0.88
        restoreTypeCardTrans2.y = 52
        restoreTypeCard3.opacity = 0
        restoreTypeCard3.scale = 0.88
        restoreTypeCardTrans3.y = 52
        typeCardsEntranceAnim.restart()
    }

    onVisibleChanged: {
        if (!visible)
            return
        root.reconcileRestoreEntry(true)
        if (root.restoreStep === 0)
            root.playTypeCardsEntrance()
    }
    onRestoreStepChanged: {
        if (root.restoreStep === 0)
            root.playTypeCardsEntrance()
    }
    Component.onCompleted: {
        root.reconcileRestoreEntry(true)
        if (root.restoreStep === 0)
            root.playTypeCardsEntrance()
        if (serviceClient.connected)
            serviceClient.refreshRepository()
    }

    // Job list may arrive after navigation; attach to Summary when an active restore appears.
    Connections {
        target: serviceClient.jobs
        function onRevisionChanged() {
            if (!root.visible)
                return
            if (root.activeRestoreAnchorMs() > 0)
                root.reconcileRestoreEntry(false)
        }
        function onCountsChanged() {
            if (!root.visible)
                return
            if (root.activeRestoreAnchorMs() > 0)
                root.reconcileRestoreEntry(false)
        }
    }

    function modeMatchesContentKind(kind) {
        var k = Number(kind || 1)
        if (root.isFileMode)
            return k === 2
        // Disk and volume restore both use volume_set recovery points.
        return k === 1
    }

    function filterCheckpointsForMode(list) {
        var out = []
        var items = list || []
        for (var i = 0; i < items.length; ++i) {
            if (root.modeMatchesContentKind(items[i].contentKind))
                out.push(items[i])
        }
        return out
    }

    readonly property int mappedCount: {
        var _e = root.mappingEpoch
        var n = 0
        if (root.isVolumeMode) {
            var vmap = root.volumeMappings || {}
            for (var vk in vmap) {
                if ((vmap[vk] || "").length > 0)
                    ++n
            }
        } else {
            var map = root.diskMappings || {}
            for (var k in map) {
                if (Number(map[k]) >= 0)
                    ++n
            }
        }
        return n
    }

    readonly property bool hasCheckpoint: root.selectedCheckpointId.length > 0
    readonly property bool hasMapping: root.mappedCount > 0
    readonly property bool canRestore: {
        if (!serviceClient.connected || serviceClient.restoreCommandBusy || !root.hasCheckpoint)
            return false
        if (root.isFileMode) {
            var entries = serviceClient.fileRecoverEntries
            var targets = serviceClient.fileRestoreTargets
            return serviceClient.fileRestoreAvailable
                   && entries && entries.selectedCount > 0
                   && targets && targets.selectedCount > 0
                   && !(entries.loading)
                   && !(targets.loading)
        }
        return serviceClient.restoreStartAvailable
               && root.hasMapping
               && !root.sourceLayoutLoading
    }
    readonly property string restoreBlockReason: {
        if (!serviceClient.connected)
            //% "Service is not connected"
            return qsTrId("aegra.error.service.disconnected")
        if (serviceClient.restoreCommandBusy)
            //% "A restore command is already in progress"
            return qsTrId("aegra.restore.busy")
        if (!root.hasCheckpoint)
            //% "Select a checkpoint first"
            return qsTrId("aegra.restore.select_checkpoint_first")
        if (root.isFileMode) {
            if (!serviceClient.fileRestoreAvailable)
                //% "Service does not support file restore"
                return qsTrId("aegra.restore.file.capability_missing")
            var entries = serviceClient.fileRecoverEntries
            var targets = serviceClient.fileRestoreTargets
            if (entries && entries.loading)
                //% "Loading archive files..."
                return qsTrId("aegra.restore.file.loading_entries")
            if (!entries || entries.selectedCount <= 0)
                //% "Select files or folders to restore"
                return qsTrId("aegra.restore.file.select_entries")
            if (!targets || targets.selectedCount <= 0)
                //% "Select a target folder"
                return qsTrId("aegra.restore.file.select_target")
            return ""
        }
        if (!serviceClient.restoreStartAvailable)
            //% "Service does not support restore"
            return qsTrId("aegra.restore.capability_missing")
        if (root.sourceLayoutLoading)
            //% "Loading source disks..."
            return qsTrId("aegra.restore.loading_source")
        if (!root.hasMapping)
            return root.isVolumeMode
                   //% "Choose “Restore to” on a source volume"
                   ? qsTrId("aegra.restore.volume_map_required")
                   //% "Choose “Restore to” on a source disk"
                   : qsTrId("aegra.restore.map_required")
        return ""
    }

    readonly property color restoreTargetBorder: "#27ae60"
    readonly property color systemDiskBorder: "#e74c3c"

    readonly property var backupDates: {
        var _dep = serviceClient.recoveryPointCount
        if (!serviceClient.recoveryPoints)
            return []
        return serviceClient.recoveryPoints.backupDateYmds()
    }

    function clearDiskMappings() {
        root.diskMappings = ({})
        root.mappingEpoch++
    }

    function clearVolumeMappings() {
        root.volumeMappings = ({})
        root.mappingEpoch++
    }

    function mappedTarget(sourceNum) {
        var _e = root.mappingEpoch
        var map = root.diskMappings || {}
        var key = String(sourceNum)
        if (map[key] === undefined || map[key] === null)
            return -1
        return Number(map[key])
    }

    function mappedTargetVolume(sourceVolumeIndex) {
        var _e = root.mappingEpoch
        var map = root.volumeMappings || {}
        var key = String(sourceVolumeIndex)
        if (map[key] === undefined || map[key] === null)
            return ""
        return String(map[key] || "")
    }

    function isTargetMappedByOther(sourceNum, targetNum) {
        if (targetNum < 0)
            return false
        var map = root.diskMappings || {}
        for (var k in map) {
            if (Number(k) !== Number(sourceNum) && Number(map[k]) === Number(targetNum))
                return true
        }
        return false
    }

    function isVolumeTargetMappedByOther(sourceVolumeIndex, targetSourceId) {
        if (!targetSourceId || targetSourceId.length === 0)
            return false
        var map = root.volumeMappings || {}
        for (var k in map) {
            if (Number(k) !== Number(sourceVolumeIndex)
                    && String(map[k] || "") === String(targetSourceId))
                return true
        }
        return false
    }

    function isDiskMappedAsTarget(diskNum) {
        var _e = root.mappingEpoch
        if (diskNum === undefined || diskNum === null)
            return false
        var map = root.diskMappings || {}
        for (var k in map) {
            if (Number(map[k]) === Number(diskNum))
                return true
        }
        return false
    }

    function isVolumeMappedAsTarget(sourceId) {
        var _e = root.mappingEpoch
        if (!sourceId || sourceId.length === 0)
            return false
        var map = root.volumeMappings || {}
        for (var k in map) {
            if (String(map[k] || "") === String(sourceId))
                return true
        }
        return false
    }

    function targetLargeEnoughForSource(sourceNum, targetNum) {
        var srcBytes = 0
        var sources = root.sourceDisks || []
        for (var i = 0; i < sources.length; ++i) {
            if (Number(sources[i].diskNumber) === Number(sourceNum)) {
                srcBytes = Number(sources[i].capacityBytes) || 0
                break
            }
        }
        var tgtBytes = 0
        var targets = root.targetDisks || []
        for (var j = 0; j < targets.length; ++j) {
            if (Number(targets[j].diskNumber) === Number(targetNum)) {
                tgtBytes = Number(targets[j].capacityBytes) || 0
                break
            }
        }
        if (srcBytes <= 0 || tgtBytes <= 0)
            return true
        var tol = 1024 * 1024
        return tgtBytes + tol >= srcBytes
    }

    function targetVolumeLargeEnough(sourceVolumeIndex, targetSourceId) {
        var srcBytes = 0
        var sources = root.sourceVolumes || []
        for (var i = 0; i < sources.length; ++i) {
            if (Number(sources[i].volumeIndex) === Number(sourceVolumeIndex)) {
                srcBytes = Number(sources[i].capacityBytes) || 0
                break
            }
        }
        var tgtBytes = 0
        var targets = root.targetVolumes || []
        for (var j = 0; j < targets.length; ++j) {
            if (String(targets[j].sourceId) === String(targetSourceId)) {
                tgtBytes = Number(targets[j].capacityBytes) || 0
                break
            }
        }
        if (srcBytes <= 0 || tgtBytes <= 0)
            return true
        var tol = 1024 * 1024
        return tgtBytes + tol >= srcBytes
    }

    /// Why a drop/map is rejected (empty = ok). Used by drag-drop highlight and setDiskMapping.
    function mappingBlockReason(sourceNum, targetNum) {
        if (sourceNum < 0 || targetNum < 0)
            return ""
        if (!root.targetLargeEnoughForSource(sourceNum, targetNum))
            //% "Target disk is smaller than the source disk"
            return qsTrId("aegra.restore.target_too_small")
        if (root.isTargetMappedByOther(sourceNum, targetNum))
            //% "That target is already mapped by another source disk"
            return qsTrId("aegra.restore.target_in_use")
        var targets = root.targetDisks || []
        for (var i = 0; i < targets.length; ++i) {
            if (Number(targets[i].diskNumber) === Number(targetNum)
                    && targets[i].isSystemDisk === true)
                //% "System disk restore requires PE (not available online)"
                return qsTrId("aegra.restore.system_target_blocked")
        }
        return ""
    }

    function volumeMappingBlockReason(sourceVolumeIndex, targetSourceId) {
        if (sourceVolumeIndex < 0 || !targetSourceId || targetSourceId.length === 0)
            return ""
        if (!root.targetVolumeLargeEnough(sourceVolumeIndex, targetSourceId))
            //% "Target volume is smaller than the source volume"
            return qsTrId("aegra.restore.volume_target_too_small")
        if (root.isVolumeTargetMappedByOther(sourceVolumeIndex, targetSourceId))
            //% "That target is already mapped by another source volume"
            return qsTrId("aegra.restore.volume_target_in_use")
        var targets = root.targetVolumes || []
        for (var i = 0; i < targets.length; ++i) {
            if (String(targets[i].sourceId) !== String(targetSourceId))
                continue
            if (targets[i].isSystem === true)
                //% "System volume restore requires PE (not available online)"
                return qsTrId("aegra.restore.volume_system_target_blocked")
            if (targets[i].isReadOnly === true)
                //% "Restore target volume is read-only"
                return qsTrId("aegra.restore.volume_target_read_only")
            break
        }
        return ""
    }

    function setDiskMapping(sourceNum, targetNum) {
        if (sourceNum < 0)
            return
        // Invalid targets are rejected silently; drag ghost already shows the reason.
        if (targetNum >= 0 && root.mappingBlockReason(sourceNum, targetNum).length > 0)
            return
        var map = Object.assign({}, root.diskMappings || {})
        map[String(sourceNum)] = targetNum
        root.diskMappings = map
        root.mappingEpoch++
    }

    function setVolumeMapping(sourceVolumeIndex, targetSourceId) {
        if (sourceVolumeIndex < 0)
            return
        var tid = targetSourceId || ""
        if (tid.length > 0
                && root.volumeMappingBlockReason(sourceVolumeIndex, tid).length > 0)
            return
        var map = Object.assign({}, root.volumeMappings || {})
        map[String(sourceVolumeIndex)] = tid
        root.volumeMappings = map
        root.mappingEpoch++
    }

    /// Options for source → target ComboBox (old RestoreBackend::targetDiskOptions).
    function targetDiskOptions(sourceNum) {
        var _e = root.mappingEpoch
        var _t = root.targetDisks
        var out = []
        out.push({
            //% "Not mapped"
            label: qsTrId("aegra.restore.not_mapped"),
            value: -1,
            tooSmall: false,
            inUse: false,
            isSystem: false,
            enabled: true
        })
        var targets = root.targetDisks || []
        for (var i = 0; i < targets.length; ++i) {
            var d = targets[i]
            var num = Number(d.diskNumber)
            var lab = d.name || ("Disk " + num)
            if (d.size)
                lab += "  (" + d.size + ")"
            var isSystem = d.isSystemDisk === true
            if (isSystem)
                //% "[System]"
                lab += "  " + qsTrId("aegra.restore.system_tag")
            var tooSmall = !root.targetLargeEnoughForSource(sourceNum, num)
            var inUse = root.isTargetMappedByOther(sourceNum, num)
            if (tooSmall)
                //% "— too small"
                lab += "  " + qsTrId("aegra.restore.target_too_small_tag")
            else if (inUse)
                //% "— in use"
                lab += "  " + qsTrId("aegra.restore.target_in_use_tag")
            else if (isSystem)
                //% "— PE only"
                lab += "  " + qsTrId("aegra.restore.pe_only_tag")
            out.push({
                label: lab,
                value: num,
                tooSmall: tooSmall,
                inUse: inUse,
                isSystem: isSystem,
                enabled: !tooSmall && !inUse && !isSystem
            })
        }
        return out
    }

    /// Default every source to Not mapped; user maps via ComboBox or drag-drop.
    function initDefaultMappings() {
        var map = ({})
        var sources = root.sourceDisks || []
        for (var j = 0; j < sources.length; ++j) {
            var sn = Number(sources[j].diskNumber)
            map[String(sn)] = -1
        }
        root.diskMappings = map
        root.mappingEpoch++
    }

    function initDefaultVolumeMappings() {
        var map = ({})
        var sources = root.sourceVolumes || []
        for (var j = 0; j < sources.length; ++j) {
            var vi = Number(sources[j].volumeIndex)
            map[String(vi)] = ""
        }
        root.volumeMappings = map
        root.mappingEpoch++
    }

    /// Queued multi-disk / multi-volume restore: one Job per mapping.
    property var pendingRestoreQueue: []
    property bool multiRestoreActive: false

    /// All source→target pairs with target >= 0, stable source-disk order.
    function allMappedPairs() {
        var pairs = []
        var sources = root.sourceDisks || []
        for (var i = 0; i < sources.length; ++i) {
            var sn = Number(sources[i].diskNumber)
            var tn = root.mappedTarget(sn)
            if (tn >= 0)
                pairs.push({ source: sn, target: tn })
        }
        return pairs
    }

    function allMappedVolumePairs() {
        var pairs = []
        var sources = root.sourceVolumes || []
        for (var i = 0; i < sources.length; ++i) {
            var vi = Number(sources[i].volumeIndex)
            var tid = root.mappedTargetVolume(vi)
            if (tid && tid.length > 0)
                pairs.push({ sourceVolumeIndex: vi, targetSourceId: tid })
        }
        return pairs
    }

    /// Options for source volume → target volume ComboBox.
    function targetVolumeOptions(sourceVolumeIndex) {
        var _e = root.mappingEpoch
        var _t = root.targetVolumes
        var out = []
        out.push({
            //% "Not mapped"
            label: qsTrId("aegra.restore.not_mapped"),
            value: "",
            tooSmall: false,
            inUse: false,
            isSystem: false,
            isReadOnly: false,
            enabled: true
        })
        var targets = root.targetVolumes || []
        for (var i = 0; i < targets.length; ++i) {
            var v = targets[i]
            var sid = String(v.sourceId || "")
            if (sid.length === 0)
                continue
            var lab = v.letter ? (v.letter + ": ") : ""
            lab += (v.name || sid)
            if (v.size)
                lab += "  (" + v.size + ")"
            var isSystem = v.isSystem === true
            var isReadOnly = v.isReadOnly === true
            if (isSystem)
                //% "[System]"
                lab += "  " + qsTrId("aegra.restore.system_tag")
            var tooSmall = !root.targetVolumeLargeEnough(sourceVolumeIndex, sid)
            var inUse = root.isVolumeTargetMappedByOther(sourceVolumeIndex, sid)
            if (tooSmall)
                //% "— too small"
                lab += "  " + qsTrId("aegra.restore.target_too_small_tag")
            else if (inUse)
                //% "— in use"
                lab += "  " + qsTrId("aegra.restore.target_in_use_tag")
            else if (isSystem)
                //% "— PE only"
                lab += "  " + qsTrId("aegra.restore.pe_only_tag")
            else if (isReadOnly)
                //% "— read-only"
                lab += "  " + qsTrId("aegra.restore.read_only_tag")
            out.push({
                label: lab,
                value: sid,
                tooSmall: tooSmall,
                inUse: inUse,
                isSystem: isSystem,
                isReadOnly: isReadOnly,
                enabled: !tooSmall && !inUse && !isSystem && !isReadOnly
            })
        }
        return out
    }

    function beginRestoreSession() {
        root.restoreSessionStartMs = Date.now()
        root.restoreJobsSubmitted = false
        root.restoreSessionFailed = false
        root.restoreSessionErrorText = ""
        root.filePreflightPending = false
        if (root.restoreStep !== 2)
                root.restoreStep = 2
    }

    function startNextQueuedRestore() {
        var q = root.pendingRestoreQueue || []
        if (q.length === 0) {
            root.multiRestoreActive = false
            // All Jobs accepted — stay on Summary and show progress.
            root.restoreJobsSubmitted = true
            return
        }
        var pair = q[0]
        // Copy remainder so QML binding sees a new array.
        root.pendingRestoreQueue = q.slice(1)
        var ok = false
        if (root.isVolumeMode) {
            ok = serviceClient.startVolumeRestore(pair.sourceVolumeIndex,
                                                  pair.targetSourceId,
                                                  root.selectedCheckpointId,
                                                  root.pendingLayoutPassword)
        } else {
            ok = serviceClient.startDiskRestore(pair.source, pair.target,
                                               root.selectedCheckpointId,
                                               root.pendingLayoutPassword,
                                               root.preserveSignature,
                                               root.autoExtend)
        }
        if (!ok) {
            root.pendingRestoreQueue = []
            root.multiRestoreActive = false
            root.restoreJobsSubmitted = true
            root.restoreSessionFailed = true
        }
    }

    function startMappedRestore() {
        // Summary step owns the Restore action; workspace only advances via Next.
        if (root.restoreStep !== 2)
            return
        if (!root.canRestore)
            return
        if (root.restoreJobsSubmitted && !root.restoreSessionFailed && !root.restoreSessionComplete)
            return
        root.beginRestoreSession()
        if (root.isFileMode) {
            // Reuses the Next-step preflight token when selection is unchanged.
            if (!serviceClient.startFileRestore(root.selectedCheckpointId,
                                                root.fileConflictPolicy,
                                                root.pendingLayoutPassword,
                                                root.fileRestoreSecurity)) {
                root.restoreJobsSubmitted = true
                root.restoreSessionFailed = true
                //% "Could not start file restore"
                root.restoreSessionErrorText = qsTrId("aegra.error.file_restore.command_failed")
            }
            return
        }
        var pairs = root.isVolumeMode ? root.allMappedVolumePairs()
                                      : root.allMappedPairs()
        if (pairs.length === 0) {
            serviceClient.showToast(root.isVolumeMode
                //% "Choose “Restore to” on a source volume"
                ? qsTrId("aegra.restore.volume_map_required")
                //% "Choose “Restore to” on a source disk"
                : qsTrId("aegra.restore.map_required"), true)
            root.restoreJobsSubmitted = true
            root.restoreSessionFailed = true
            root.restoreSessionErrorText = root.isVolumeMode
                ? qsTrId("aegra.restore.volume_map_required")
                : qsTrId("aegra.restore.map_required")
            return
        }
        root.pendingRestoreQueue = pairs
        root.multiRestoreActive = true
        root.startNextQueuedRestore()
    }

    Connections {
        target: serviceClient
        function onRestorePreflightSucceeded() {
            if (!root.filePreflightPending)
                return
            root.filePreflightPending = false
            root.clearRestoreSession()
            root.restoreStep = 2
        }
        function onRestorePreflightFailed(message) {
            root.filePreflightPending = false
            // Stay on workspace; red toast already shown by ServiceClient.
        }
        function onRestoreStartSucceeded() {
            if (root.multiRestoreActive) {
                // Start the next mapped disk (or mark submitted when the queue is empty).
                root.startNextQueuedRestore()
                return
            }
            // Single-job (file) path — stay on Summary.
            root.restoreJobsSubmitted = true
        }
        function onRestoreStartFailed(message) {
            root.pendingRestoreQueue = []
            root.multiRestoreActive = false
            root.restoreJobsSubmitted = true
            root.restoreSessionFailed = true
            root.restoreSessionErrorText = message || ""
        }
    }

    function openCheckpointPanel() {
        if (serviceClient.connected)
            serviceClient.refreshRepository()
        // Reset selection state; parent always owns open/selectedDate (no child writes).
        root.panelSelectedDate = ""
        root.panelCheckpoints = []
        root.panelCheckpointsEpoch++
        root.checkpointPanelOpen = true
    }

    function reloadPanelCheckpoints() {
        if (!serviceClient.recoveryPoints || root.panelSelectedDate.length === 0) {
            root.panelCheckpoints = []
        } else {
            root.panelCheckpoints = root.filterCheckpointsForMode(
                    serviceClient.recoveryPoints.checkpointsForDate(root.panelSelectedDate))
        }
        root.panelCheckpointsEpoch++
    }

    function onPanelDateSelected(dateStr) {
        if (!dateStr || dateStr.length === 0)
            return
        root.panelSelectedDate = dateStr
        root.reloadPanelCheckpoints()
    }

    property string pendingLayoutPassword: ""

    function applySelectedCheckpoint(item) {
        if (!item) {
            root.selectedCheckpointId = ""
            root.selectedCheckpointLabel = ""
            root.selectedContentKind = 1
            root.pendingLayoutPassword = ""
            root.clearDiskMappings()
            root.clearVolumeMappings()
            serviceClient.loadRecoveryPointLayout("")
            // Drop prior file-restore tree / target selection so Done → Files starts clean.
            if (typeof serviceClient.clearFileRestoreState === "function")
                serviceClient.clearFileRestoreState()
            return
        }
        var kind = Number(item.contentKind || 1)
        if (!root.modeMatchesContentKind(kind)) {
            //% "This checkpoint does not match the selected restore type"
            serviceClient.showToast(qsTrId("aegra.restore.type_mismatch"), true)
            return
        }
        root.selectedCheckpointId = item.fileUuid || ""
        root.selectedContentKind = kind
        root.pendingLayoutPassword = ""
        root.clearDiskMappings()
        root.clearVolumeMappings()
        var bits = []
        if (item.createdText)
            bits.push(item.createdText)
        else {
            if (root.panelSelectedDate)
                bits.push(root.panelSelectedDate)
            if (item.timeText)
                bits.push(item.timeText)
        }
        if (item.backupType)
            bits.push(item.backupType)
        if (item.sizeText)
            bits.push(item.sizeText)
        if (kind === 2)
            //% "Files"
            bits.push(qsTrId("aegra.restore.mode_files"))
        else if (root.isVolumeMode)
            //% "Volume"
            bits.push(qsTrId("aegra.restore.mode_volume"))
        else
            //% "Disk"
            bits.push(qsTrId("aegra.restore.mode_disk"))
        root.selectedCheckpointLabel = bits.join("  ·  ")
        if (root.isFileMode) {
            serviceClient.loadFileRecoverRoots(root.selectedCheckpointId, "")
            serviceClient.loadFileRestoreTargetRoots()
            return
        }
        // Disk / volume: same volume_set layout; mapping UI differs by restoreMode.
        serviceClient.loadRecoveryPointLayout(root.selectedCheckpointId, "")
    }

    function submitLayoutPassword(password) {
        if (!root.selectedCheckpointId || root.selectedCheckpointId.length === 0)
            return
        root.pendingLayoutPassword = password || ""
        root.clearDiskMappings()
        root.clearVolumeMappings()
        if (root.selectedContentKind === 2) {
            serviceClient.loadFileRecoverRoots(root.selectedCheckpointId,
                                               root.pendingLayoutPassword)
            serviceClient.loadFileRestoreTargetRoots()
            return
        }
        serviceClient.loadRecoveryPointLayout(root.selectedCheckpointId, root.pendingLayoutPassword)
    }

    Connections {
        target: serviceClient
        function onRecoveryPointLayoutChanged() {
            if (serviceClient.recoveryPointLayoutLoading)
                return
            if (serviceClient.recoveryPointSourceDisks
                    && serviceClient.recoveryPointSourceDisks.length > 0) {
                // Layout loaded — reset mappings for the active restore mode.
                if (root.isVolumeMode)
                    root.initDefaultVolumeMappings()
                else
                    root.initDefaultMappings()
                return
            }
            if (root.selectedCheckpointId.length === 0)
                return
            // Layout failed: likely encrypted archive needing a password.
            if (serviceClient.recoveryPointLayoutErrorText
                    && serviceClient.recoveryPointLayoutErrorText.length > 0) {
                passwordDialog.errorText = serviceClient.recoveryPointLayoutErrorText
                passwordDialog.open()
            }
        }
        function onInventoryChanged() {
            // Re-validate defaults when target inventory arrives after layout.
            if (serviceClient.recoveryPointSourceDisks
                    && serviceClient.recoveryPointSourceDisks.length > 0
                    && root.mappedCount === 0) {
                if (root.isVolumeMode)
                    root.initDefaultVolumeMappings()
                else
                    root.initDefaultMappings()
            }
        }
    }

    BackupPasswordDialog {
        id: passwordDialog
        parent: Overlay.overlay
        onAccepted: function(password) { root.submitLayoutPassword(password) }
        onCancelled: { root.pendingLayoutPassword = "" }
    }

    Connections {
        target: serviceClient
        function onRepositoryChanged() {
            // After RP list arrives, refresh calendar dates and any open date selection.
            if (root.checkpointPanelOpen)
                root.reloadPanelCheckpoints()
        }
    }

    /// Volumes for bar (ratio by capacityBytes); optional trailing free/unallocated.
    function displayVolumesForDisk(diskData) {
        var raw = (diskData && diskData.volumes) ? diskData.volumes : []
        var list = []
        var sumBytes = 0
        for (var i = 0; i < raw.length; ++i) {
            var v = raw[i]
            if (!v)
                continue
            var tb = Number(v.capacityBytes) || 0
            list.push({
                letter: v.letter || "",
                name: v.name || "",
                size: v.size || "",
                fileSystem: v.fs || v.fileSystem || "",
                totalBytes: tb,
                unallocated: false
            })
            sumBytes += tb
        }
        var diskTotal = diskData
                        ? (Number(diskData.capacityBytes) || 0) : 0
        if (diskTotal <= 0 && sumBytes > 0)
            diskTotal = sumBytes
        for (var j = 0; j < list.length; ++j) {
            var r = diskTotal > 0 ? (list[j].totalBytes / diskTotal) : (1.0 / Math.max(1, list.length))
            list[j].ratio = r > 0 ? r : 0.04
        }
        if (diskTotal > 0 && sumBytes > 0 && diskTotal > sumBytes + 1024 * 1024) {
            var unallocBytes = diskTotal - sumBytes
            list.push({
                letter: "",
                //% "Unallocated"
                name: qsTrId("aegra.restore.unallocated"),
                size: "",
                fileSystem: "",
                ratio: unallocBytes / diskTotal,
                totalBytes: unallocBytes,
                unallocated: true
            })
        }
        return list
    }

    // Shared disk row — icon + name + proportional partition bar (old DiskRow).
    // Source rows: drag body onto a Target row, or use "Restore to" ComboBox.
    // Target rows: DropArea accepts source-disk drags.
    component DiskRow: Rectangle {
        id: rowRoot
        property var diskData: ({})
        property bool showSystem: false
        property bool showMapping: false
        property bool highlightAsTarget: false
        readonly property int diskNumber: diskData && diskData.diskNumber !== undefined
                                          ? Number(diskData.diskNumber) : -1
        /// While a valid source→target drop is hovered (no red fill for invalid targets).
        property bool dropHover: false
        property bool dropAccepted: false
        width: parent ? parent.width : 100
        height: showMapping ? 96 : 68
        radius: 6
        color: Theme.colorListItem
        border.width: {
            if (rowRoot.dropHover && rowRoot.dropAccepted) return 2
            if (rowRoot.highlightAsTarget) return 2
            return 0
        }
        border.color: {
            if (rowRoot.dropHover && rowRoot.dropAccepted) return Theme.colorAccentBlue
            if (rowRoot.highlightAsTarget) return root.restoreTargetBorder
            return "transparent"
        }

        property var displayVolumes: root.displayVolumesForDisk(diskData)

        // Drag a source disk onto a target (ComboBox remains available below).
        MouseArea {
            id: sourceDragMouse
            anchors.fill: parent
            anchors.margins: 2
            anchors.bottomMargin: rowRoot.showMapping ? 34 : 2
            z: 20
            enabled: rowRoot.showMapping && rowRoot.diskNumber >= 0
            hoverEnabled: true
            cursorShape: {
                if (!enabled)
                    return Qt.ArrowCursor
                return pressed || drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            }
            drag.target: dragProxy
            drag.threshold: 6
            drag.axis: Drag.XAndYAxis
            onPressed: function(mouse) {
                root.clearMappingDropBlocked()
                var overlay = Overlay.overlay
                if (!overlay)
                    return
                dragProxy.parent = overlay
                var g = mapToItem(overlay, mouse.x, mouse.y)
                dragProxy.x = g.x - dragProxy.Drag.hotSpot.x
                dragProxy.y = g.y - dragProxy.Drag.hotSpot.y
            }
            onReleased: {
                if (dragProxy.Drag.active)
                    dragProxy.Drag.drop()
                root.clearMappingDropBlocked()
            }
            onCanceled: root.clearMappingDropBlocked()
        }

        // Ghost follows the cursor while dragging (reparented to Overlay on press).
        Rectangle {
            id: dragProxy
            // Wider when blocked so ban icon + reason fit on one line.
            width: root.mappingDropBlocked ? 320 : 180
            height: 36
            radius: 6
            z: 100000
            visible: Drag.active
            opacity: 0.92
            color: root.mappingDropBlocked ? Theme.colorAccentRed : Theme.colorAccentBlue
            border.width: 1
            border.color: Theme.colorBorder
            // Carried into DropArea via drag.source
            property int sourceDiskNumber: rowRoot.diskNumber
            Drag.keys: ["aegra.restore.sourceDisk"]
            Drag.active: sourceDragMouse.drag.active
            Drag.hotSpot.x: width / 2
            Drag.hotSpot.y: height / 2
            Drag.source: dragProxy

            Row {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8

                // Prohibition mark: circle with diagonal slash.
                Item {
                    width: 18
                    height: 18
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.mappingDropBlocked
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: Theme.colorTextWhite
                        border.width: 2
                        border.color: Theme.colorAccentRed
                    }
                    Rectangle {
                        width: parent.width * 0.78
                        height: 2
                        radius: 1
                        color: Theme.colorAccentRed
                        anchors.centerIn: parent
                        rotation: -45
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                           - (root.mappingDropBlocked ? 18 + parent.spacing : 0)
                    elide: Text.ElideRight
                    // Blocked: reason after the ban icon. Otherwise: source disk name.
                    text: {
                        if (root.mappingDropBlocked && root.mappingDropBlockReason.length > 0)
                            return root.mappingDropBlockReason
                        var n = dragProxy.sourceDiskNumber
                        if (rowRoot.diskData && rowRoot.diskData.name)
                            return rowRoot.diskData.name
                        return "Disk " + n
                    }
                    color: Theme.colorTextWhite
                    font.pixelSize: root.mappingDropBlocked ? 11 : 12
                    font.bold: true
                    font.family: Theme.fontFamily
                }
            }
        }

        // Target drop zone: map source_disk → this disk.
        DropArea {
            id: targetDrop
            anchors.fill: parent
            z: 15
            keys: ["aegra.restore.sourceDisk"]
            enabled: !rowRoot.showMapping && rowRoot.diskNumber >= 0
            onEntered: function(drag) {
                var src = -1
                if (drag.source && drag.source.sourceDiskNumber !== undefined)
                    src = Number(drag.source.sourceDiskNumber)
                var reason = (src >= 0)
                             ? root.mappingBlockReason(src, rowRoot.diskNumber) : ""
                var ok = src >= 0 && reason.length === 0
                // Always accept so onDropped can map valid targets (invalid: silent no-op).
                rowRoot.dropHover = ok
                rowRoot.dropAccepted = ok
                root.setMappingDropFeedback(!ok, reason)
                drag.accept(Qt.CopyAction)
            }
            onPositionChanged: function(drag) {
                var src = -1
                if (drag.source && drag.source.sourceDiskNumber !== undefined)
                    src = Number(drag.source.sourceDiskNumber)
                var reason = (src >= 0)
                             ? root.mappingBlockReason(src, rowRoot.diskNumber) : ""
                var ok = src >= 0 && reason.length === 0
                rowRoot.dropHover = ok
                rowRoot.dropAccepted = ok
                root.setMappingDropFeedback(!ok, reason)
                drag.accept(Qt.CopyAction)
            }
            onExited: {
                rowRoot.dropHover = false
                rowRoot.dropAccepted = false
                root.clearMappingDropBlocked()
            }
            onDropped: function(drop) {
                rowRoot.dropHover = false
                rowRoot.dropAccepted = false
                root.clearMappingDropBlocked()
                var src = -1
                if (drop.source && drop.source.sourceDiskNumber !== undefined)
                    src = Number(drop.source.sourceDiskNumber)
                drop.acceptProposedAction()
                if (src < 0 || rowRoot.diskNumber < 0)
                    return
                // Invalid mapping: silent reject (reason already shown on drag ghost).
                root.setDiskMapping(src, rowRoot.diskNumber)
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 10

                DiskIcon {
                    Layout.alignment: Qt.AlignVCenter
                    size: 28
                    variant: (rowRoot.diskData && rowRoot.diskData.isSystemDisk) ? "system" : "hdd"
                }

                Column {
                    Layout.preferredWidth: 100
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 1
                    Text {
                        text: (rowRoot.diskData && rowRoot.diskData.name)
                              ? rowRoot.diskData.name : ""
                        color: Theme.colorTextWhite
                        font.pixelSize: 12
                        font.bold: true
                        font.family: Theme.fontFamily
                    }
                    Text {
                        text: {
                            if (!rowRoot.diskData)
                                return ""
                            var style = rowRoot.diskData.partitionStyle
                                        || rowRoot.diskData.type || ""
                            if (style.indexOf("GPT") >= 0 || style.indexOf("MBR") >= 0)
                                return "Basic (" + (style.indexOf("GPT") >= 0 ? "GPT" : "MBR") + ")"
                            return style.length > 0 ? style : "Basic (GPT)"
                        }
                        color: Theme.colorTextGrey
                        font.pixelSize: 10
                        font.family: Theme.fontFamily
                    }
                    Text {
                        text: (rowRoot.diskData && rowRoot.diskData.size)
                              ? rowRoot.diskData.size : ""
                        color: Theme.colorTextGrey
                        font.pixelSize: 10
                        font.family: Theme.fontFamily
                    }
                }

                Rectangle {
                    id: barBg
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    Layout.alignment: Qt.AlignVCenter
                    radius: 3
                    color: Theme.colorInput
                    border.width: 1
                    border.color: Theme.colorBorder
                    clip: true

                    Row {
                        id: partsRow
                        anchors.fill: parent
                        anchors.margins: 1
                        spacing: 1

                        Repeater {
                            model: rowRoot.displayVolumes
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                property bool isUnalloc: modelData && modelData.unallocated === true
                                height: partsRow.height
                                width: {
                                    var n = rowRoot.displayVolumes.length
                                    var gap = Math.max(0, n - 1) * partsRow.spacing
                                    var avail = Math.max(0, partsRow.width - gap)
                                    var r = modelData && modelData.ratio ? modelData.ratio : 0
                                    if (r <= 0)
                                        r = 0.04
                                    var w = Math.floor(avail * r)
                                    return Math.max(isUnalloc ? 8 : 12, w)
                                }
                                radius: 2
                                color: isUnalloc ? Theme.colorUnallocated
                                                 : Theme.colorAccentBlue
                                border.width: 0
                                opacity: isUnalloc ? 1.0 : 0.45
                                clip: true

                                Canvas {
                                    anchors.fill: parent
                                    visible: isUnalloc
                                    onPaint: {
                                        var ctx = getContext("2d")
                                        var w = width
                                        var h = height
                                        if (w < 1 || h < 1)
                                            return
                                        ctx.clearRect(0, 0, w, h)
                                        ctx.strokeStyle = Theme.colorUnallocatedHatch
                                        ctx.lineWidth = 1
                                        var step = 6
                                        for (var x = -h; x < w + h; x += step) {
                                            ctx.beginPath()
                                            ctx.moveTo(x, h)
                                            ctx.lineTo(x + h, 0)
                                            ctx.stroke()
                                        }
                                    }
                                    onWidthChanged: requestPaint()
                                    onHeightChanged: requestPaint()
                                    Component.onCompleted: requestPaint()
                                }

                                Text {
                                    anchors.centerIn: parent
                                    width: parent.width - 4
                                    horizontalAlignment: Text.AlignHCenter
                                    z: 1
                                    text: {
                                        if (!modelData)
                                            return ""
                                        if (isUnalloc) {
                                            if (parent.width < 48)
                                                return "…"
                                            return modelData.name || ""
                                        }
                                        var title = modelData.letter || modelData.name || ""
                                        var sz = modelData.size || ""
                                        var fs = modelData.fileSystem || ""
                                        if (parent.width < 50)
                                            return title
                                        return title + "\n" + sz + (fs ? (" " + fs) : "")
                                    }
                                    color: isUnalloc ? Theme.colorUnallocatedText
                                                     : Theme.colorVolumeText
                                    font.pixelSize: parent.width < 60 ? 9 : 10
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    wrapMode: Text.WordWrap
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: rowRoot.displayVolumes.length === 0
                        //% "No partitions"
                        text: qsTrId("aegra.restore.no_partitions")
                        color: Theme.colorTextGrey
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                    }
                }
            }

            // Source → Target mapping row (old RestorePage "Restore to" ComboBox).
            RowLayout {
                Layout.fillWidth: true
                visible: rowRoot.showMapping
                spacing: 8

                Text {
                    //% "Restore to"
                    text: qsTrId("aegra.restore.restore_to")
                    color: Theme.colorTextGrey
                    font.pixelSize: 11
                    font.family: Theme.fontFamily
                }

                ComboBox {
                    id: mapCombo
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    property int sourceNum: rowRoot.diskData && rowRoot.diskData.diskNumber !== undefined
                                            ? Number(rowRoot.diskData.diskNumber) : -1
                    model: {
                        var _e = root.mappingEpoch
                        var _t = root.targetDisks
                        return root.targetDiskOptions(mapCombo.sourceNum)
                    }
                    textRole: "label"

                    function syncIndex() {
                        var want = root.mappedTarget(mapCombo.sourceNum)
                        if (!model)
                            return
                        for (var i = 0; i < model.length; ++i) {
                            if (Number(model[i].value) === Number(want)) {
                                currentIndex = i
                                return
                            }
                        }
                        currentIndex = 0
                    }

                    Component.onCompleted: syncIndex()
                    onModelChanged: Qt.callLater(syncIndex)

                    onActivated: function(index) {
                        if (index < 0 || !model || index >= model.length) {
                            syncIndex()
                            return
                        }
                        var item = model[index]
                        if (!item || item.enabled === false
                                || item.tooSmall || item.inUse || item.isSystem) {
                            syncIndex()
                            return
                        }
                        var val = Number(item.value)
                        root.setDiskMapping(mapCombo.sourceNum, val)
                        Qt.callLater(syncIndex)
                    }

                    background: Rectangle {
                        color: Theme.colorInput
                        radius: 8
                        border.width: 1
                        border.color: Theme.colorBorder
                    }
                    indicator: ComboBoxIndicator { combo: mapCombo }
                    contentItem: Text {
                        leftPadding: 8
                        rightPadding: mapCombo.indicator ? mapCombo.indicator.width + 12 : 8
                        text: mapCombo.displayText
                        color: Theme.colorTextWhite
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    popup: Popup {
                        y: mapCombo.height
                        width: mapCombo.width
                        implicitHeight: Math.min(contentItem.implicitHeight + 4, 220)
                        padding: 2
                        contentItem: ListView {
                            clip: true
                            implicitHeight: contentHeight
                            model: mapCombo.popup.visible ? mapCombo.delegateModel : null
                            currentIndex: mapCombo.highlightedIndex
                            ScrollIndicator.vertical: ScrollIndicator { }
                        }
                        background: Rectangle {
                            color: Theme.colorPopup
                            radius: 8
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                    }
                    delegate: ItemDelegate {
                        id: mapItemDel
                        width: mapCombo.width
                        height: 28
                        hoverEnabled: true
                        enabled: modelData && modelData.enabled !== false
                        highlighted: mapCombo.highlightedIndex === index
                        contentItem: Text {
                            text: modelData ? modelData.label : ""
                            color: parent.enabled ? Theme.colorTextWhite : Theme.colorTextGrey
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 4
                            color: (mapItemDel.hovered || mapItemDel.highlighted) ? Theme.colorHover : "transparent"
                        }
                    }
                }
            }
        }
    }


    // Volume row — source (map + drag) or target (drop).
    component VolumeRow: Rectangle {
        id: volRoot
        property var volumeData: ({})
        property bool showMapping: false
        property bool highlightAsTarget: false
        readonly property int volumeIndex: volumeData && volumeData.volumeIndex !== undefined
                                           ? Number(volumeData.volumeIndex) : -1
        readonly property string sourceId: volumeData && volumeData.sourceId
                                          ? String(volumeData.sourceId) : ""
        property bool dropHover: false
        property bool dropAccepted: false
        width: parent ? parent.width : 100
        height: showMapping ? 72 : 52
        radius: 6
        color: Theme.colorListItem
        border.width: {
            if (volRoot.dropHover && volRoot.dropAccepted) return 2
            if (volRoot.highlightAsTarget) return 2
            return 0
        }
        border.color: {
            if (volRoot.dropHover && volRoot.dropAccepted) return Theme.colorAccentBlue
            if (volRoot.highlightAsTarget) return root.restoreTargetBorder
            return "transparent"
        }

        // Capacity fill bar (used/total ratio)
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: {
                var cap = Number(volRoot.volumeData ? volRoot.volumeData.capacityBytes : 0) || 0
                var free = Number(volRoot.volumeData ? volRoot.volumeData.freeBytes : 0)
                if (isNaN(free) || free < 0) free = 0
                if (cap <= 0) return 0
                var ratio = Math.min(1, Math.max(0, (cap - free) / cap))
                return parent.width * ratio
            }
            color: Theme.colorAccentBlue
            opacity: 0.18
            radius: parent.radius
        }

        MouseArea {
            id: volDragMouse
            anchors.fill: parent
            anchors.margins: 2
            anchors.bottomMargin: volRoot.showMapping ? 30 : 2
            z: 20
            enabled: volRoot.showMapping && volRoot.volumeIndex >= 0
            hoverEnabled: true
            cursorShape: {
                if (!enabled)
                    return Qt.ArrowCursor
                return pressed || drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            }
            drag.target: volDragProxy
            drag.threshold: 6
            drag.axis: Drag.XAndYAxis
            onPressed: function(mouse) {
                root.clearMappingDropBlocked()
                var overlay = Overlay.overlay
                if (!overlay)
                    return
                volDragProxy.parent = overlay
                var g = mapToItem(overlay, mouse.x, mouse.y)
                volDragProxy.x = g.x - volDragProxy.Drag.hotSpot.x
                volDragProxy.y = g.y - volDragProxy.Drag.hotSpot.y
            }
            onReleased: {
                if (volDragProxy.Drag.active)
                    volDragProxy.Drag.drop()
                root.clearMappingDropBlocked()
            }
            onCanceled: root.clearMappingDropBlocked()
        }

        Rectangle {
            id: volDragProxy
            width: root.mappingDropBlocked ? 320 : 200
            height: 36
            radius: 6
            z: 100000
            visible: Drag.active
            opacity: 0.92
            color: root.mappingDropBlocked ? Theme.colorAccentRed : Theme.colorAccentBlue
            border.width: 1
            border.color: Theme.colorBorder
            property int sourceVolumeIndex: volRoot.volumeIndex
            Drag.keys: ["aegra.restore.sourceVolume"]
            Drag.active: volDragMouse.drag.active
            Drag.hotSpot.x: width / 2
            Drag.hotSpot.y: height / 2
            Drag.source: volDragProxy

            Row {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8
                Item {
                    width: 18
                    height: 18
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.mappingDropBlocked
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: Theme.colorTextWhite
                        border.width: 2
                        border.color: Theme.colorAccentRed
                    }
                    Rectangle {
                        width: parent.width * 0.78
                        height: 2
                        radius: 1
                        color: Theme.colorAccentRed
                        anchors.centerIn: parent
                        rotation: -45
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - (root.mappingDropBlocked ? 18 + parent.spacing : 0)
                    elide: Text.ElideRight
                    text: {
                        if (root.mappingDropBlocked && root.mappingDropBlockReason.length > 0)
                            return root.mappingDropBlockReason
                        if (volRoot.volumeData && volRoot.volumeData.title)
                            return volRoot.volumeData.title
                        return qsTrId("aegra.restore.source_volume").arg(volRoot.volumeIndex)
                    }
                    color: Theme.colorTextWhite
                    font.pixelSize: root.mappingDropBlocked ? 11 : 12
                    font.bold: true
                    font.family: Theme.fontFamily
                }
            }
        }

        DropArea {
            id: volDrop
            anchors.fill: parent
            z: 15
            keys: ["aegra.restore.sourceVolume"]
            enabled: !volRoot.showMapping && volRoot.sourceId.length > 0
            onEntered: function(drag) {
                var src = -1
                if (drag.source && drag.source.sourceVolumeIndex !== undefined)
                    src = Number(drag.source.sourceVolumeIndex)
                var reason = (src >= 0)
                             ? root.volumeMappingBlockReason(src, volRoot.sourceId) : ""
                var ok = src >= 0 && reason.length === 0
                volRoot.dropHover = ok
                volRoot.dropAccepted = ok
                root.setMappingDropFeedback(!ok, reason)
                drag.accept(Qt.CopyAction)
            }
            onPositionChanged: function(drag) {
                var src = -1
                if (drag.source && drag.source.sourceVolumeIndex !== undefined)
                    src = Number(drag.source.sourceVolumeIndex)
                var reason = (src >= 0)
                             ? root.volumeMappingBlockReason(src, volRoot.sourceId) : ""
                var ok = src >= 0 && reason.length === 0
                volRoot.dropHover = ok
                volRoot.dropAccepted = ok
                root.setMappingDropFeedback(!ok, reason)
                drag.accept(Qt.CopyAction)
            }
            onExited: {
                volRoot.dropHover = false
                volRoot.dropAccepted = false
                root.clearMappingDropBlocked()
            }
            onDropped: function(drop) {
                volRoot.dropHover = false
                volRoot.dropAccepted = false
                root.clearMappingDropBlocked()
                var src = -1
                if (drop.source && drop.source.sourceVolumeIndex !== undefined)
                    src = Number(drop.source.sourceVolumeIndex)
                drop.acceptProposedAction()
                if (src < 0 || volRoot.sourceId.length === 0)
                    return
                root.setVolumeMapping(src, volRoot.sourceId)
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Rectangle {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    radius: 4
                    color: Theme.colorInput
                    border.width: 1
                    border.color: Theme.colorBorder
                    Text {
                        anchors.centerIn: parent
                        text: volRoot.showMapping
                              ? String(volRoot.volumeIndex)
                              : (volRoot.volumeData && volRoot.volumeData.letter
                                 ? String(volRoot.volumeData.letter) : "V")
                        color: Theme.colorAccentBlue
                        font.pixelSize: 11
                        font.bold: true
                        font.family: Theme.fontFamily
                    }
                }
                Column {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: {
                            if (!volRoot.volumeData)
                                return ""
                            if (volRoot.showMapping)
                                return volRoot.volumeData.title
                                       || qsTrId("aegra.restore.source_volume").arg(volRoot.volumeIndex)
                            var lab = volRoot.volumeData.letter
                                      ? (volRoot.volumeData.letter + " ") : ""
                            return lab + (volRoot.volumeData.name || volRoot.sourceId)
                        }
                        color: Theme.colorTextWhite
                        font.pixelSize: 12
                        font.bold: true
                        font.family: Theme.fontFamily
                    }
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: {
                            if (!volRoot.volumeData)
                                return ""
                            var bits = []
                            if (volRoot.volumeData.size)
                                bits.push(volRoot.volumeData.size)
                            var fs = volRoot.volumeData.fs || volRoot.volumeData.fileSystem || ""
                            if (fs)
                                bits.push(fs)
                            if (volRoot.volumeData.isSystem === true)
                                bits.push(qsTrId("aegra.restore.system_tag"))
                            return bits.join("  ·  ")
                        }
                        color: Theme.colorTextGrey
                        font.pixelSize: 10
                        font.family: Theme.fontFamily
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: volRoot.showMapping
                spacing: 8
                Text {
                    //% "Restore to"
                    text: qsTrId("aegra.restore.restore_to")
                    color: Theme.colorTextGrey
                    font.pixelSize: 11
                    font.family: Theme.fontFamily
                }
                ComboBox {
                    id: volMapCombo
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    property int sourceVol: volRoot.volumeIndex
                    model: {
                        var _e = root.mappingEpoch
                        var _t = root.targetVolumes
                        return root.targetVolumeOptions(volMapCombo.sourceVol)
                    }
                    textRole: "label"
                    function syncIndex() {
                        var want = root.mappedTargetVolume(volMapCombo.sourceVol)
                        if (!model)
                            return
                        for (var i = 0; i < model.length; ++i) {
                            if (String(model[i].value || "") === String(want)) {
                                currentIndex = i
                                return
                            }
                        }
                        currentIndex = 0
                    }
                    Component.onCompleted: syncIndex()
                    onModelChanged: Qt.callLater(syncIndex)
                    onActivated: function(index) {
                        if (index < 0 || !model || index >= model.length) {
                            syncIndex()
                            return
                        }
                        var item = model[index]
                        if (!item || item.enabled === false
                                || item.tooSmall || item.inUse
                                || item.isSystem || item.isReadOnly) {
                            syncIndex()
                            return
                        }
                        root.setVolumeMapping(volMapCombo.sourceVol, String(item.value || ""))
                        Qt.callLater(syncIndex)
                    }
                    background: Rectangle {
                        color: Theme.colorInput
                        radius: 8
                        border.width: 1
                        border.color: Theme.colorBorder
                    }
                    indicator: ComboBoxIndicator { combo: volMapCombo }
                    contentItem: Text {
                        leftPadding: 8
                        rightPadding: volMapCombo.indicator ? volMapCombo.indicator.width + 12 : 8
                        text: volMapCombo.displayText
                        color: Theme.colorTextWhite
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    popup: Popup {
                        y: volMapCombo.height
                        width: volMapCombo.width
                        implicitHeight: Math.min(contentItem.implicitHeight + 4, 220)
                        padding: 2
                        contentItem: ListView {
                            clip: true
                            implicitHeight: contentHeight
                            model: volMapCombo.popup.visible ? volMapCombo.delegateModel : null
                            currentIndex: volMapCombo.highlightedIndex
                            ScrollIndicator.vertical: ScrollIndicator { }
                        }
                        background: Rectangle {
                            color: Theme.colorPopup
                            radius: 8
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                    }
                    delegate: ItemDelegate {
                        id: volMapItemDel
                        width: volMapCombo.width
                        height: 28
                        hoverEnabled: true
                        enabled: modelData && modelData.enabled !== false
                        highlighted: volMapCombo.highlightedIndex === index
                        contentItem: Text {
                            text: modelData ? modelData.label : ""
                            color: parent.enabled ? Theme.colorTextWhite : Theme.colorTextGrey
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 4
                            color: (volMapItemDel.hovered || volMapItemDel.highlighted) ? Theme.colorHover : "transparent"
                        }
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 40
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.bottomMargin: 16
        spacing: 12

        // Header row A: stat cards — visible only on step 0
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: root.restoreStep === 0 ? 80 : 0
            visible: root.restoreStep === 0
            spacing: 12

            // Stat card: Volume Sets
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                radius: 14
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0; color: Theme.colorCard }
                    GradientStop { position: 1.0; color: Theme.colorCardEnd }
                }
                border.width: 1
                border.color: Theme.colorBorder

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18
                    spacing: 14

                    Rectangle {
                        width: 40; height: 40; radius: 12
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#3B82F6" }
                            GradientStop { position: 1.0; color: "#2563EB" }
                        }
                        DiskIcon { anchors.centerIn: parent; size: 22; variant: "hdd" }
                    }
                    ColumnLayout {
                        spacing: 2
                        Layout.fillWidth: true
                        Text {
                            //% "Volume Sets"
                            text: qsTrId("aegra.restore.stat.volume_sets")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                        }
                        Text {
                            text: serviceClient.recoveryPoints
                                  ? serviceClient.recoveryPoints.volumeSetCount.toString()
                                  : "0"
                            color: Theme.colorTextWhite
                            font.pixelSize: 22; font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }
                }
            }

            // Stat card: File Sets
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                radius: 14
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0; color: Theme.colorCard }
                    GradientStop { position: 1.0; color: Theme.colorCardEnd }
                }
                border.width: 1
                border.color: Theme.colorBorder

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18
                    spacing: 14

                    Rectangle {
                        width: 40; height: 40; radius: 12
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#10B981" }
                            GradientStop { position: 1.0; color: "#059669" }
                        }
                        FolderIcon { anchors.centerIn: parent; size: 22 }
                    }
                    ColumnLayout {
                        spacing: 2
                        Layout.fillWidth: true
                        Text {
                            //% "File Sets"
                            text: qsTrId("aegra.restore.stat.file_sets")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                        }
                        Text {
                            text: serviceClient.recoveryPoints
                                  ? serviceClient.recoveryPoints.fileSetCount.toString()
                                  : "0"
                            color: Theme.colorTextWhite
                            font.pixelSize: 22; font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }
                }
            }
        }

        // Header row B: back button + step progress bar — visible on steps 1 and 2
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: root.restoreStep > 0 ? 64 : 0
            visible: root.restoreStep > 0
            spacing: 8

            Item {
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                Layout.alignment: Qt.AlignTop
                z: 20

                readonly property bool backEnabled: !root.restoreSessionRunning
                                                    && !root.restoreProgressSucceeded

                Rectangle {
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 32; height: 32; radius: 16
                    opacity: parent.backEnabled ? 1 : 0
                    color: restoreBackMouse.containsMouse && parent.backEnabled
                           ? Theme.colorHover : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: "\u25C0"
                        font.pixelSize: 11
                        color: Theme.colorTextGrey
                    }
                }
                MouseArea {
                    id: restoreBackMouse
                    anchors.fill: parent
                    enabled: parent.backEnabled
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    //% "Back"
                    Accessible.name: qsTrId("aegra.common.back")
                    onPressed: function(mouse) {
                        mouse.accepted = true
                        root.stepBarBack()
                    }
                }
            }

            Item {
                id: restoreStepBar
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                Layout.alignment: Qt.AlignVCenter

                readonly property int stepCount: 2
                readonly property real slotW: width / stepCount
                readonly property real lineY: 14
                readonly property real lineLeft: slotW * 0.5
                readonly property real lineSpan: slotW * (stepCount - 1)

                Rectangle {
                    x: restoreStepBar.lineLeft; y: restoreStepBar.lineY
                    width: restoreStepBar.lineSpan; height: 3; radius: 1.5
                    color: Theme.colorProgressTrack
                }
                Rectangle {
                    x: restoreStepBar.lineLeft; y: restoreStepBar.lineY
                    width: restoreStepBar.lineSpan
                         * ((root.restoreStep - 1) / Math.max(1, restoreStepBar.stepCount - 1))
                    height: 3; radius: 1.5
                    color: Theme.colorAccentBlue
                    Behavior on width { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
                }

                Repeater {
                    model: restoreStepBar.stepCount
                    delegate: Item {
                        width: restoreStepBar.slotW
                        height: restoreStepBar.height
                        x: index * restoreStepBar.slotW
                        y: 0

                        readonly property bool done: index < (root.restoreStep - 1)
                        readonly property bool current: index === (root.restoreStep - 1)

                        Rectangle {
                            id: restoreStepDot
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 2; width: 26; height: 26; radius: 13
                            border.width: (parent.done || parent.current) ? 0 : 2
                            border.color: Theme.colorBorder
                            color: (parent.done || parent.current)
                                   ? Theme.colorAccentBlue : Theme.colorCard
                            Behavior on color { ColorAnimation { duration: 200 } }
                            Text {
                                anchors.centerIn: parent
                                text: parent.parent.done ? "\u2713" : ("" + (index + 1))
                                color: (parent.parent.done || parent.parent.current)
                                       ? "#ffffff" : Theme.colorTextDim
                                font.pixelSize: parent.parent.done ? 12 : 11
                                font.bold: true; font.family: Theme.fontFamily
                            }
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: restoreStepDot.bottom
                            anchors.topMargin: 6
                            width: parent.width - 4
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: root.restoreStepLabels[index] || ""
                            color: parent.current ? Theme.colorTextWhite
                                   : (parent.done ? Theme.colorAccentBlue : Theme.colorTextDim)
                            font.pixelSize: 11
                            font.bold: parent.current
                            font.family: Theme.fontFamily
                        }
                    }
                }
            }
        }

        // Steps share a clipped container so transitions slide like Backup.
        Item {
            id: restoreStepContainer
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

        // -------- Step 0: restore type cards (Disk / Volume / Files) --------
        Item {
            id: restoreStep0
            width: parent.width
            height: parent.height
            visible: opacity > 0.001
            opacity: root.restoreStep === 0 ? 1 : 0
            x: root.restoreStep === 0 ? 0
               : (root.restoreStep > 0 ? -restoreStepContainer.width
                                       : restoreStepContainer.width)
            Behavior on opacity { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
            Behavior on x { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }

            ColumnLayout {
                anchors.fill: parent
                spacing: 24

                Item { height: 8 }

                ColumnLayout {
                    id: restoreTypeHeader
                    spacing: 6
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    opacity: 0
                    transform: Translate {
                        id: restoreTypeHeaderTrans
                        y: 18
                    }
                    Text {
                        //% "Choose restore type"
                        text: qsTrId("aegra.restore.type_title")
                        color: Theme.colorTextWhite
                        font.pixelSize: 22
                        font.bold: true
                        font.family: Theme.fontFamily
                        Layout.alignment: Qt.AlignHCenter
                    }
                    Text {
                        //% "Select whole-disk recovery, volume-to-volume restore, or file and folder restore from a recovery point"
                        text: qsTrId("aegra.restore.type_subtitle")
                        color: Theme.colorTextGrey
                        font.pixelSize: 13
                        font.family: Theme.fontFamily
                        Layout.alignment: Qt.AlignHCenter
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        Layout.fillWidth: true
                        Layout.maximumWidth: 720
                    }
                }

                Item { height: 4 }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignHCenter | Qt.AlignTop
                    spacing: 20

                    // Card: Disk restore
                    Rectangle {
                        id: restoreTypeCard1
                        Layout.fillWidth: true
                        Layout.preferredWidth: 280
                        Layout.maximumWidth: 340
                        implicitHeight: 300
                        radius: Theme.radiusCard
                        gradient: Gradient {
                            orientation: Gradient.Vertical
                            GradientStop { position: 0.0; color: Theme.colorCard }
                            GradientStop { position: 1.0; color: Theme.colorCardEnd }
                        }
                        border.width: 1
                        border.color: diskTypeMouse.containsMouse ? Theme.colorAccentBlue : Theme.colorBorder
                        opacity: 0
                        scale: 0.88
                        transformOrigin: Item.Center
                        transform: Translate {
                            id: restoreTypeCardTrans1
                            y: 52
                        }
                        MouseArea {
                            id: diskTypeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectRestoreType("disk")
                        }
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 22
                            spacing: 14
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
                                //% "Disk restore"
                                text: qsTrId("aegra.restore.type.disk_title")
                                color: Theme.colorTextWhite
                                font.pixelSize: 17
                                font.bold: true
                                font.family: Theme.fontFamily
                                Layout.alignment: Qt.AlignHCenter
                            }
                            Text {
                                //% "Restore an entire physical disk from a volume-set recovery point onto a target disk (layout, partitions, and options)."
                                text: qsTrId("aegra.restore.type.disk_desc")
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
                                    //% "Choose Disk restore →"
                                    text: qsTrId("aegra.restore.type.disk_action")
                                    color: "#ffffff"
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                }
                            }
                        }
                    }

                    // Card: Volume restore
                    Rectangle {
                        id: restoreTypeCard2
                        Layout.fillWidth: true
                        Layout.preferredWidth: 280
                        Layout.maximumWidth: 340
                        implicitHeight: 300
                        radius: Theme.radiusCard
                        gradient: Gradient {
                            orientation: Gradient.Vertical
                            GradientStop { position: 0.0; color: Theme.colorCard }
                            GradientStop { position: 1.0; color: Theme.colorCardEnd }
                        }
                        border.width: 1
                        border.color: volumeTypeMouse.containsMouse ? "#6366F1" : Theme.colorBorder
                        opacity: 0
                        scale: 0.88
                        transformOrigin: Item.Center
                        transform: Translate {
                            id: restoreTypeCardTrans2
                            y: 52
                        }
                        MouseArea {
                            id: volumeTypeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectRestoreType("volume")
                        }
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 22
                            spacing: 14
                            Rectangle {
                                width: 56
                                height: 56
                                radius: 18
                                Layout.alignment: Qt.AlignHCenter
                                gradient: Gradient {
                                    GradientStop { position: 0.0; color: "#818CF8" }
                                    GradientStop { position: 1.0; color: "#4F46E5" }
                                }
                                DiskIcon { anchors.centerIn: parent; size: 34; variant: "hdd" }
                            }
                            Text {
                                //% "Volume restore"
                                text: qsTrId("aegra.restore.type.volume_title")
                                color: Theme.colorTextWhite
                                font.pixelSize: 17
                                font.bold: true
                                font.family: Theme.fontFamily
                                Layout.alignment: Qt.AlignHCenter
                            }
                            Text {
                                //% "Map one backup volume onto an existing non-system volume of equal or larger size without rewriting the whole disk."
                                text: qsTrId("aegra.restore.type.volume_desc")
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
                                color: "#4F46E5"
                                Text {
                                    anchors.centerIn: parent
                                    //% "Choose Volume restore →"
                                    text: qsTrId("aegra.restore.type.volume_action")
                                    color: "#ffffff"
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                }
                            }
                        }
                    }

                    // Card: Files / folders restore
                    Rectangle {
                        id: restoreTypeCard3
                        Layout.fillWidth: true
                        Layout.preferredWidth: 280
                        Layout.maximumWidth: 340
                        implicitHeight: 300
                        radius: Theme.radiusCard
                        gradient: Gradient {
                            orientation: Gradient.Vertical
                            GradientStop { position: 0.0; color: Theme.colorCard }
                            GradientStop { position: 1.0; color: Theme.colorCardEnd }
                        }
                        border.width: 1
                        border.color: filesTypeMouse.containsMouse ? Theme.colorGreen : Theme.colorBorder
                        opacity: 0
                        scale: 0.88
                        transformOrigin: Item.Center
                        transform: Translate {
                            id: restoreTypeCardTrans3
                            y: 52
                        }
                        MouseArea {
                            id: filesTypeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: serviceClient.fileRestoreAvailable === true
                            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                            onClicked: root.selectRestoreType("files")
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: 20
                            color: "#80000000"
                            visible: serviceClient.fileRestoreAvailable !== true
                            z: 5
                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 32
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                //% "File restore is not available on this Service"
                                text: qsTrId("aegra.restore.file.capability_missing")
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
                        }
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 22
                            spacing: 14
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
                                //% "Files / folders"
                                text: qsTrId("aegra.restore.type.files_title")
                                color: Theme.colorTextWhite
                                font.pixelSize: 17
                                font.bold: true
                                font.family: Theme.fontFamily
                                Layout.alignment: Qt.AlignHCenter
                            }
                            Text {
                                //% "Selectively restore files and folders from a file-set recovery point into a chosen target directory."
                                text: qsTrId("aegra.restore.type.files_desc")
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
                                    //% "Choose Files restore →"
                                    text: qsTrId("aegra.restore.type.files_action")
                                    color: "#ffffff"
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                }
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }
        } // restoreStep0

        // -------- Step 1: Source + Target (left) | Options (right) --------
        Item {
            id: restoreStep1
            width: parent.width
            height: parent.height
            visible: opacity > 0.001
            opacity: root.restoreStep === 1 ? 1 : 0
            x: root.restoreStep === 1 ? 0
               : (root.restoreStep < 1 ? restoreStepContainer.width
                                       : -restoreStepContainer.width)
            Behavior on opacity { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
            Behavior on x { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }

        RowLayout {
            id: mainSplitRow
            anchors.fill: parent
            spacing: 0

            ColumnLayout {
                id: disksColumn
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: root.optionsCollapsed
                                       ? 1
                                       : Math.round(1000 * (1.0 - root.optionsPaneRatio))
                Layout.minimumWidth: 280
                spacing: 0

                // -------- Source Disks --------
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: Math.round(1000 * root.sourceTargetRatio)
                    Layout.minimumHeight: 120
                    color: Theme.colorCard
                    radius: Theme.radiusCard
                    border.width: 0

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Rectangle {
                                width: 3
                                height: 16
                                color: Theme.colorAccentBlue
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Text {
                                text: root.isFileMode
                                      //% "Archive files"
                                      ? qsTrId("aegra.restore.file.source_title")
                                      : (root.isVolumeMode
                                         //% "Source Volumes"
                                         ? qsTrId("aegra.restore.source_volumes")
                                         //% "Source Disks"
                                         : qsTrId("aegra.restore.source_disks"))
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                text: root.isFileMode
                                      //% "(expand folders and check items to restore)"
                                      ? qsTrId("aegra.restore.file.source_hint")
                                      : (root.isVolumeMode
                                         //% "(drag onto a target volume, or use Restore to)"
                                         ? qsTrId("aegra.restore.source_volume_hint")
                                         //% "(drag onto a target disk, or use Restore to)"
                                         : qsTrId("aegra.restore.source_hint"))
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            // Same card-action link style as Backup "+ Add"
                            Text {
                                id: selectCheckpointLink
                                //% "Select checkpoint"
                                text: "+ " + qsTrId("aegra.restore.select_checkpoint")
                                color: selectCheckpointHover.containsMouse
                                       ? Theme.colorLinkHover : Theme.colorAccentBlue
                                font.pixelSize: 13
                                font.bold: true
                                font.family: Theme.fontFamily
                                Layout.alignment: Qt.AlignVCenter
                                MouseArea {
                                    id: selectCheckpointHover
                                    anchors.fill: parent
                                    anchors.margins: -6
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.openCheckpointPanel()
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: root.selectedCheckpointLabel.length > 0
                            //% "Selected:"
                            text: qsTrId("aegra.restore.selected_label")
                                  + " " + root.selectedCheckpointLabel
                            color: Theme.colorAccentBlue
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                            elide: Text.ElideMiddle
                        }

                        ListView {
                            id: fileRecoverList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 0
                            visible: root.isFileMode
                            ScrollBar.vertical: ScrollBar {
                                policy: ScrollBar.AsNeeded
                            }
                            model: serviceClient.fileRecoverEntries
                            delegate: Rectangle {
                                required property string entryId
                                required property string displayName
                                required property bool hasChildren
                                required property bool isDirectory
                                required property int depth
                                required property bool expanded
                                required property bool nodeLoading
                                required property int checkState
                                width: fileRecoverList.width
                                height: 26
                                radius: 4
                                color: "transparent"
                                // Hover highlight
                                Rectangle {
                                    anchors.fill: parent
                                    radius: parent.radius
                                    color: Theme.colorHover
                                    opacity: archiveRowHover.containsMouse ? 1.0 : 0.0
                                    Behavior on opacity { NumberAnimation { duration: 120 } }
                                }
                                MouseArea {
                                    id: archiveRowHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    acceptedButtons: Qt.NoButton
                                }
                                // Expand on row click (checkbox keeps its own hit target).
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: hasChildren
                                    z: 0
                                    cursorShape: hasChildren ? Qt.PointingHandCursor
                                                             : Qt.ArrowCursor
                                    onClicked: serviceClient.fileRecoverEntries
                                                   .toggleExpanded(entryId)
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8 + Math.max(0, depth) * 14
                                    anchors.rightMargin: 8
                                    spacing: 8
                                    z: 1
                                    Text {
                                        text: hasChildren ? (expanded ? "▾" : "▸") : " "
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 12
                                        Layout.preferredWidth: 14
                                    }
                                    Rectangle {
                                        width: 16
                                        height: 16
                                        radius: 3
                                        color: checkState === 2
                                               ? Theme.colorAccentBlue
                                               : (checkState === 1
                                                  ? Theme.colorAccentBlue : "transparent")
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
                                            z: 2
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: function(mouse) {
                                                mouse.accepted = true
                                                serviceClient.fileRecoverEntries
                                                    .toggleChecked(entryId)
                                            }
                                        }
                                    }
                                    Loader {
                                        Layout.preferredWidth: 16
                                        Layout.preferredHeight: 16
                                        Layout.alignment: Qt.AlignVCenter
                                        sourceComponent: isDirectory
                                                         ? restoreFolderIconComponent
                                                         : restoreFileIconComponent
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: displayName + (nodeLoading ? " …" : "")
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                        elide: Text.ElideMiddle
                                    }
                                }
                            }
                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 24
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                visible: fileRecoverList.count === 0
                                text: {
                                    if (!root.hasCheckpoint)
                                        return qsTrId("aegra.restore.select_checkpoint_source")
                                    if (serviceClient.fileRecoverEntries
                                            && serviceClient.fileRecoverEntries.loading)
                                        return qsTrId("aegra.restore.file.loading_entries")
                                    if (serviceClient.fileRecoverEntries
                                            && serviceClient.fileRecoverEntries.errorText)
                                        return serviceClient.fileRecoverEntries.errorText
                                    //% "No files in this recovery point"
                                    return qsTrId("aegra.restore.file.empty_entries")
                                }
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
                        }
                        ListView {
                            id: sourceList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 10
                            visible: !root.isVolumeMode && !root.isFileMode
                            model: root.sourceDisks
                            delegate: DiskRow {
                                required property var modelData
                                width: sourceList.width
                                diskData: modelData
                                showSystem: false
                                showMapping: true
                            }
                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 32
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                visible: sourceList.count === 0
                                text: {
                                    if (root.sourceLayoutLoading)
                                        //% "Loading source disks..."
                                        return qsTrId("aegra.restore.loading_source")
                                    if (root.sourceLayoutError.length > 0)
                                        return root.sourceLayoutError
                                    if (root.selectedCheckpointId.length > 0)
                                        //% "No source volumes in this checkpoint"
                                        return qsTrId("aegra.restore.no_source_volumes")
                                    //% "Select a checkpoint to view source disks"
                                    return qsTrId("aegra.restore.select_checkpoint_source")
                                }
                                color: root.sourceLayoutError.length > 0
                                       ? Theme.colorAccentRed : Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
                        }
                        ListView {
                            id: sourceVolumeList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 8
                            visible: root.isVolumeMode && !root.isFileMode
                            model: root.sourceVolumes
                            delegate: VolumeRow {
                                required property var modelData
                                width: sourceVolumeList.width
                                volumeData: modelData
                                showMapping: true
                            }
                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 32
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                visible: sourceVolumeList.count === 0
                                text: {
                                    if (root.sourceLayoutLoading)
                                        //% "Loading source disks..."
                                        return qsTrId("aegra.restore.loading_source")
                                    if (root.sourceLayoutError.length > 0)
                                        return root.sourceLayoutError
                                    if (root.selectedCheckpointId.length > 0)
                                        //% "No source volumes in this checkpoint"
                                        return qsTrId("aegra.restore.no_source_volumes")
                                    //% "Select a checkpoint to view source volumes"
                                    return qsTrId("aegra.restore.select_checkpoint_source_volumes")
                                }
                                color: root.sourceLayoutError.length > 0
                                       ? Theme.colorAccentRed : Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
                        }
                    }
                }

                // Vertical splitter Source / Target
                Item {
                    id: diskSplitter
                    Layout.fillWidth: true
                    Layout.preferredHeight: 5
                    Layout.minimumHeight: 5
                    z: 2
                    Rectangle {
                        anchors.centerIn: parent
                        width: Math.min(36, parent.width - 24)
                        height: 2
                        radius: 1
                        color: diskSplitMouse.pressed || diskSplitMouse.containsMouse
                               ? Theme.colorAccentBlue : Theme.colorBorder
                        opacity: diskSplitMouse.pressed || diskSplitMouse.containsMouse ? 1.0 : 0.7
                    }
                    MouseArea {
                        id: diskSplitMouse
                        anchors.fill: parent
                        anchors.topMargin: -3
                        anchors.bottomMargin: -3
                        hoverEnabled: true
                        cursorShape: Qt.SplitVCursor
                        preventStealing: true
                        property real pressY: 0
                        property real pressRatio: 0.5
                        onPressed: function(mouse) {
                            pressY = mapToItem(disksColumn, 0, mouse.y).y
                            pressRatio = root.sourceTargetRatio
                        }
                        onPositionChanged: function(mouse) {
                            if (!pressed)
                                return
                            var y = mapToItem(disksColumn, 0, mouse.y).y
                            var avail = disksColumn.height - diskSplitter.height
                            if (avail < 80)
                                return
                            var r = pressRatio + (y - pressY) / avail
                            root.sourceTargetRatio = Math.min(0.8, Math.max(0.2, r))
                        }
                    }
                }

                // -------- Target Disks --------
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: Math.round(1000 * (1.0 - root.sourceTargetRatio))
                    Layout.minimumHeight: 120
                    color: Theme.colorCard
                    radius: Theme.radiusCard
                    border.width: 0

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Rectangle {
                                width: 3
                                height: 16
                                color: Theme.colorAccentBlue
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Text {
                                text: root.isFileMode
                                      //% "Target folder"
                                      ? qsTrId("aegra.restore.file.target_title")
                                      : (root.isVolumeMode
                                         //% "Target Volumes"
                                         ? qsTrId("aegra.restore.target_volumes")
                                         //% "Target Disks"
                                         : qsTrId("aegra.restore.target_disks"))
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                text: root.isFileMode
                                      //% "(choose one directory on this PC)"
                                      ? qsTrId("aegra.restore.file.target_hint")
                                      : (root.isVolumeMode
                                         //% "(this PC — drop a source volume here)"
                                         ? qsTrId("aegra.restore.target_volume_hint")
                                         //% "(this PC — drop a source disk here)"
                                         : qsTrId("aegra.restore.target_hint"))
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }

                        ListView {
                            id: fileTargetList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 0
                            visible: root.isFileMode
                            ScrollBar.vertical: ScrollBar {
                                policy: ScrollBar.AsNeeded
                            }
                            model: serviceClient.fileRestoreTargets
                            delegate: Rectangle {
                                required property string nodeToken
                                required property string displayName
                                required property bool hasChildren
                                required property bool isDirectory
                                required property int depth
                                required property bool expanded
                                required property bool nodeLoading
                                required property int checkState
                                required property bool isSelectable
                                width: fileTargetList.width
                                height: 26
                                radius: 4
                                color: "transparent"
                                opacity: isSelectable ? 1.0 : 0.55
                                clip: true
                                // Hover highlight
                                Rectangle {
                                    anchors.fill: parent
                                    radius: parent.radius
                                    color: Theme.colorHover
                                    opacity: targetRowHover.containsMouse ? 1.0 : 0.0
                                    Behavior on opacity { NumberAnimation { duration: 120 } }
                                }
                                MouseArea {
                                    id: targetRowHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    acceptedButtons: Qt.NoButton
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8 + Math.max(0, depth) * 14
                                    anchors.rightMargin: 8
                                    spacing: 8
                                    z: 1
                                    Text {
                                        text: hasChildren ? (expanded ? "▾" : "▸") : " "
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 12
                                        Layout.preferredWidth: 14
                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: hasChildren
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: serviceClient.fileRestoreTargets
                                                           .toggleExpanded(nodeToken)
                                        }
                                    }
                                    Rectangle {
                                        width: 16
                                        height: 16
                                        radius: 3
                                        visible: isSelectable && isDirectory
                                        color: checkState === 2
                                               ? Theme.colorGreen : "transparent"
                                        border.width: 2
                                        border.color: checkState === 2
                                                      ? Theme.colorGreen
                                                      : Theme.colorTextGrey
                                        Text {
                                            anchors.centerIn: parent
                                            text: checkState === 2 ? "\u2713" : ""
                                            color: "white"
                                            font.pixelSize: 11
                                            font.bold: true
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: serviceClient.fileRestoreTargets
                                                           .toggleChecked(nodeToken)
                                        }
                                    }
                                    Loader {
                                        Layout.preferredWidth: 16
                                        Layout.preferredHeight: 16
                                        Layout.alignment: Qt.AlignVCenter
                                        sourceComponent: root.restoreTargetIconFor(
                                            depth, isDirectory, displayName)
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: displayName + (nodeLoading ? " …" : "")
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                        elide: Text.ElideMiddle
                                    }
                                }
                            }
                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 24
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                visible: fileTargetList.count === 0
                                text: {
                                    if (serviceClient.fileRestoreTargets
                                            && serviceClient.fileRestoreTargets.loading)
                                        return qsTrId("aegra.file.browse.loading")
                                    if (serviceClient.fileRestoreTargets
                                            && serviceClient.fileRestoreTargets.errorText)
                                        return serviceClient.fileRestoreTargets.errorText
                                    //% "Expand a drive and choose one target folder"
                                    return qsTrId("aegra.restore.file.target_empty")
                                }
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
                        }

                        ListView {
                            id: targetList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 10
                            visible: !root.isVolumeMode && !root.isFileMode
                            boundsBehavior: Flickable.StopAtBounds
                            readonly property bool needsScroll: contentHeight > height + 1
                            model: root.targetDisks
                            delegate: DiskRow {
                                required property var modelData
                                width: targetList.width - (targetList.needsScroll ? 12 : 0)
                                diskData: modelData
                                showSystem: true
                                highlightAsTarget: root.isDiskMappedAsTarget(
                                                       modelData ? modelData.diskNumber : -1)
                            }
                            ScrollBar.vertical: ScrollBar {
                                policy: targetList.needsScroll ? ScrollBar.AlwaysOn
                                                               : ScrollBar.AlwaysOff
                                width: 8
                            }
                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 24
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                visible: targetList.count === 0
                                //% "Local disks will appear when inventory is available"
                                text: qsTrId("aegra.restore.target_empty")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
                        }
                        ListView {
                            id: targetVolumeList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 8
                            visible: root.isVolumeMode && !root.isFileMode
                            boundsBehavior: Flickable.StopAtBounds
                            readonly property bool needsScroll: contentHeight > height + 1
                            model: root.targetVolumes
                            delegate: VolumeRow {
                                required property var modelData
                                width: targetVolumeList.width - (targetVolumeList.needsScroll ? 12 : 0)
                                volumeData: modelData
                                showMapping: false
                                highlightAsTarget: root.isVolumeMappedAsTarget(
                                                       modelData ? modelData.sourceId : "")
                            }
                            ScrollBar.vertical: ScrollBar {
                                policy: targetVolumeList.needsScroll ? ScrollBar.AlwaysOn
                                                                     : ScrollBar.AlwaysOff
                                width: 8
                            }
                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 24
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                visible: targetVolumeList.count === 0
                                //% "Local volumes will appear when inventory is available"
                                text: qsTrId("aegra.restore.target_volume_empty")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
                        }
                    }
                }
            }

            // Horizontal drag handle
            Item {
                Layout.fillHeight: true
                Layout.preferredWidth: root.optionsCollapsed ? 0 : 5
                Layout.minimumWidth: root.optionsCollapsed ? 0 : 5
                visible: !root.optionsCollapsed
                z: 2
                Rectangle {
                    anchors.centerIn: parent
                    width: 2
                    height: Math.min(36, parent.height - 24)
                    radius: 1
                    color: optSplitMouse.pressed || optSplitMouse.containsMouse
                           ? Theme.colorAccentBlue : Theme.colorBorder
                }
                MouseArea {
                    id: optSplitMouse
                    anchors.fill: parent
                    anchors.leftMargin: -3
                    anchors.rightMargin: -3
                    hoverEnabled: true
                    cursorShape: Qt.SplitHCursor
                    preventStealing: true
                    property real pressX: 0
                    property real pressRatio: 0.3
                    onPressed: function(mouse) {
                        pressX = mapToItem(mainSplitRow, mouse.x, 0).x
                        pressRatio = root.optionsPaneRatio
                    }
                    onPositionChanged: function(mouse) {
                        if (!pressed || root.optionsCollapsed)
                            return
                        var x = mapToItem(mainSplitRow, mouse.x, 0).x
                        var avail = mainSplitRow.width
                        if (avail < 200)
                            return
                        var r = pressRatio - (x - pressX) / avail
                        root.optionsPaneRatio = Math.min(0.45, Math.max(0.18, r))
                    }
                }
            }

            // -------- Options (right, collapsible) --------
            Rectangle {
                id: optionsPanel
                Layout.fillHeight: true
                Layout.fillWidth: false
                Layout.preferredWidth: root.optionsCollapsed
                                       ? root.optionsCollapsedWidth
                                       : Math.round(1000 * root.optionsPaneRatio)
                Layout.minimumWidth: root.optionsCollapsed ? root.optionsCollapsedWidth : 180
                Layout.maximumWidth: root.optionsCollapsed
                                     ? root.optionsCollapsedWidth
                                     : parent.width * 0.48
                color: Theme.colorCard
                radius: Theme.radiusCard
                border.width: 0
                clip: true

                // Collapsed strip
                Item {
                    anchors.fill: parent
                    visible: root.optionsCollapsed
                    z: 2
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.optionsCollapsed = false
                    }
                    Column {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        anchors.topMargin: 10
                        spacing: 10
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "\u25C0"
                            color: Theme.colorTextWhite
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Item {
                            width: 16
                            height: optsCollapsedLabel.implicitWidth
                            anchors.horizontalCenter: parent.horizontalCenter
                            Text {
                                id: optsCollapsedLabel
                                anchors.centerIn: parent
                                //% "Options"
                                text: qsTrId("aegra.restore.options")
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.bold: true
                                font.family: Theme.fontFamily
                                rotation: -90
                                transformOrigin: Item.Center
                            }
                        }
                    }
                }

                // Expanded
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8
                    visible: !root.optionsCollapsed
                    enabled: !root.optionsCollapsed

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            width: 3
                            height: 16
                            color: Theme.colorAccentBlue
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Text {
                            //% "Options"
                            text: qsTrId("aegra.restore.options")
                            color: Theme.colorTextWhite
                            font.pixelSize: 14
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                            radius: 4
                            color: optsCollapseMouse.containsMouse
                                   ? Theme.colorHover : "transparent"
                            border.width: optsCollapseMouse.containsMouse ? 1 : 0
                            border.color: Theme.colorBorder
                            Text {
                                anchors.centerIn: parent
                                text: "\u25B6"
                                color: Theme.colorTextWhite
                                font.pixelSize: 11
                                font.bold: true
                            }
                            MouseArea {
                                id: optsCollapseMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.optionsCollapsed = true
                            }
                        }
                    }

                    // --- Disk full-disk options (not volume, not file_set) ---
                    CheckBox {
                        id: preserveBox
                        Layout.fillWidth: true
                        visible: !root.isVolumeMode && !root.isFileMode
                        //% "Preserve disk signature"
                        text: qsTrId("aegra.restore.preserve_signature")
                        checked: root.preserveSignature
                        onToggled: root.preserveSignature = checked
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        spacing: 10
                        indicator: Rectangle {
                            implicitWidth: 18
                            implicitHeight: 18
                            x: preserveBox.leftPadding
                            y: parent.height / 2 - height / 2
                            radius: 3
                            color: preserveBox.checked ? Theme.colorAccentBlue : Theme.colorInput
                            border.width: 1
                            border.color: Theme.colorBorder
                            Text {
                                anchors.centerIn: parent
                                text: preserveBox.checked ? "\u2713" : ""
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.bold: true
                            }
                        }
                        contentItem: Text {
                            text: preserveBox.text
                            color: Theme.colorTextWhite
                            font: preserveBox.font
                            leftPadding: preserveBox.indicator.width + preserveBox.spacing
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: !root.isVolumeMode && !root.isFileMode
                        //% "Keep MBR signature / GPT DiskId (recommended for bootable disks). Uncheck only when cloning a data disk while the source remains online."
                        text: qsTrId("aegra.restore.preserve_signature_hint")
                        color: Theme.colorTextGrey
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                    }

                    CheckBox {
                        id: extendBox
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        visible: !root.isVolumeMode && !root.isFileMode
                        //% "Auto expand last partition"
                        text: qsTrId("aegra.restore.auto_extend")
                        checked: root.autoExtend
                        onToggled: root.autoExtend = checked
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        spacing: 10
                        indicator: Rectangle {
                            implicitWidth: 18
                            implicitHeight: 18
                            x: extendBox.leftPadding
                            y: parent.height / 2 - height / 2
                            radius: 3
                            color: extendBox.checked ? Theme.colorAccentBlue : Theme.colorInput
                            border.width: 1
                            border.color: Theme.colorBorder
                            Text {
                                anchors.centerIn: parent
                                text: extendBox.checked ? "\u2713" : ""
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.bold: true
                            }
                        }
                        contentItem: Text {
                            text: extendBox.text
                            color: Theme.colorTextWhite
                            font: extendBox.font
                            leftPadding: extendBox.indicator.width + extendBox.spacing
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: !root.isVolumeMode && !root.isFileMode
                        //% "When the target disk is larger than the source, grow the last data partition (and filesystem) into free space so no large unallocated region remains. Uncheck to leave free space unallocated. Note: FAT/FAT32 volumes cannot be auto-expanded (Windows does not support online extend); free space stays unallocated."
                        text: qsTrId("aegra.restore.auto_extend_hint")
                        color: Theme.colorTextGrey
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: root.isVolumeMode
                        //% "Volume restore writes one backup volume onto an existing non-system volume of equal or larger size. Partition layout is not changed."
                        text: qsTrId("aegra.restore.volume_options_hint")
                        color: Theme.colorTextGrey
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                    }

                    // --- File-set restore options ---
                    CheckBox {
                        id: restoreSecurityBox
                        Layout.fillWidth: true
                        visible: root.isFileMode
                        //% "Restore security (ACL)"
                        text: qsTrId("aegra.restore.file.restore_security")
                        checked: root.fileRestoreSecurity
                        onToggled: root.fileRestoreSecurity = checked
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        spacing: 10
                        indicator: Rectangle {
                            implicitWidth: 18
                            implicitHeight: 18
                            x: restoreSecurityBox.leftPadding
                            y: parent.height / 2 - height / 2
                            radius: 3
                            color: restoreSecurityBox.checked ? Theme.colorAccentBlue
                                                              : Theme.colorInput
                            border.width: 1
                            border.color: Theme.colorBorder
                            Text {
                                anchors.centerIn: parent
                                text: restoreSecurityBox.checked ? "\u2713" : ""
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.bold: true
                            }
                        }
                        contentItem: Text {
                            text: restoreSecurityBox.text
                            color: Theme.colorTextWhite
                            font: restoreSecurityBox.font
                            leftPadding: restoreSecurityBox.indicator.width
                                         + restoreSecurityBox.spacing
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.isFileMode
                        //% "Apply Owner, Group, DACL, and SACL from the archive. FAT32 targets require this option to be off and cannot store files larger than 4 GiB - 1. Timestamps and attributes are still restored."
                        text: qsTrId("aegra.restore.file.restore_security_hint")
                        color: Theme.colorTextGrey
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 12
                        visible: root.isFileMode
                        //% "If a file already exists"
                        text: qsTrId("aegra.restore.file.conflict_policy")
                        color: Theme.colorTextWhite
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        font.bold: true
                        wrapMode: Text.WordWrap
                    }
                    ComboBox {
                        id: fileConflictCombo
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        visible: root.isFileMode
                        model: [
                            //% "Skip (fail on conflict)"
                            qsTrId("aegra.restore.file.conflict_fail"),
                            //% "Replace existing"
                            qsTrId("aegra.restore.file.conflict_replace"),
                            //% "Rename restored file"
                            qsTrId("aegra.restore.file.conflict_rename")
                        ]
                        // policy values are 1-based; ComboBox index is 0-based
                        currentIndex: Math.max(0, Math.min(2, root.fileConflictPolicy - 1))
                        onActivated: root.fileConflictPolicy = index + 1
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        background: Rectangle {
                            color: Theme.colorInput
                            radius: 8
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                        indicator: ComboBoxIndicator { combo: fileConflictCombo }
                        contentItem: Text {
                            leftPadding: 10
                            rightPadding: fileConflictCombo.indicator
                                          ? fileConflictCombo.indicator.width + 12 : 10
                            text: fileConflictCombo.displayText
                            color: Theme.colorTextWhite
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        popup: Popup {
                            y: fileConflictCombo.height + 2
                            width: fileConflictCombo.width
                            implicitHeight: Math.min(contentItem.implicitHeight + 4, 160)
                            padding: 2
                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                model: fileConflictCombo.popup.visible
                                       ? fileConflictCombo.delegateModel : null
                                currentIndex: fileConflictCombo.highlightedIndex
                                ScrollIndicator.vertical: ScrollIndicator { }
                            }
                            background: Rectangle {
                                color: Theme.colorPopup
                                radius: 8
                                border.width: 1
                                border.color: Theme.colorBorder
                            }
                        }
                        delegate: ItemDelegate {
                            id: conflictItemDel
                            width: fileConflictCombo.width
                            height: 30
                            hoverEnabled: true
                            highlighted: fileConflictCombo.highlightedIndex === index
                            contentItem: Text {
                                text: modelData
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 4
                                color: (conflictItemDel.hovered || conflictItemDel.highlighted) ? Theme.colorHover : "transparent"
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.isFileMode
                        //% "Choose what happens when the target path already has a file with the same name."
                        text: qsTrId("aegra.restore.file.conflict_policy_hint")
                        color: Theme.colorTextGrey
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        } // mainSplitRow
        } // restoreStep1

        // -------- Step 2: Summary + restore progress --------
        Item {
            id: restoreStep2
            width: parent.width
            height: parent.height
            visible: opacity > 0.001
            opacity: root.restoreStep === 2 ? 1 : 0
            x: root.restoreStep === 2 ? 0
               : (root.restoreStep < 2 ? restoreStepContainer.width
                                       : -restoreStepContainer.width)
            Behavior on opacity { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
            Behavior on x { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }

            Flickable {
                anchors.fill: parent
                contentWidth: width
                contentHeight: summaryOuterCol.implicitHeight + 24
                clip: true
                boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: summaryOuterCol
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(parent.width - 48, 560)
                y: 12
                spacing: 16

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    //% "Restore summary"
                    text: qsTrId("aegra.restore.summary.title")
                    color: Theme.colorTextWhite
                    font.pixelSize: 22
                    font.bold: true
                    font.family: Theme.fontFamily
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: 12
                    color: Theme.colorCard
                    border.width: 1
                    border.color: Theme.colorBorder
                    implicitHeight: summaryInfoCol.implicitHeight + 32

                    ColumnLayout {
                        id: summaryInfoCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 16
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                //% "Type"
                                text: qsTrId("aegra.restore.summary.type_label")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                Layout.preferredWidth: 140
                            }
                            Text {
                                text: root.restoreModeTitle
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.bold: true
                                font.family: Theme.fontFamily
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                //% "Checkpoint"
                                text: qsTrId("aegra.restore.summary.checkpoint_label")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                Layout.preferredWidth: 140
                            }
                            Text {
                                text: root.selectedCheckpointLabel.length > 0
                                      ? root.selectedCheckpointLabel
                                      : root.selectedCheckpointId
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: !root.isFileMode
                            Text {
                                //% "Mappings"
                                text: qsTrId("aegra.restore.summary.mappings_label")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                Layout.preferredWidth: 140
                            }
                            Text {
                                text: "" + root.mappedCount
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                Layout.fillWidth: true
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: root.isFileMode
                            Text {
                                //% "Files selected"
                                text: qsTrId("aegra.restore.summary.files_selected")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                Layout.preferredWidth: 140
                            }
                            Text {
                                text: {
                                    var entries = serviceClient.fileRecoverEntries
                                    return entries ? ("" + entries.selectedCount) : "0"
                                }
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                Layout.fillWidth: true
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: root.isFileMode
                            Text {
                                //% "Target folder"
                                text: qsTrId("aegra.restore.summary.target_folder")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                Layout.preferredWidth: 140
                            }
                            Text {
                                text: {
                                    var t = serviceClient.fileRestoreTargets
                                    return (t && t.selectionSummary) ? t.selectionSummary : "—"
                                }
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                            }
                        }
                    }
                }

                // Options card — mirrors the Options pane choices for this restore mode.
                Rectangle {
                    Layout.fillWidth: true
                    radius: 12
                    color: Theme.colorCard
                    border.width: 1
                    border.color: Theme.colorBorder
                    implicitHeight: summaryOptionsCol.implicitHeight + 32

                    ColumnLayout {
                        id: summaryOptionsCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 16
                        spacing: 10

                        Text {
                            //% "Options"
                            text: qsTrId("aegra.restore.options")
                            color: Theme.colorTextWhite
                            font.pixelSize: 14
                            font.bold: true
                            font.family: Theme.fontFamily
                        }

                        // Disk options
                        RowLayout {
                            Layout.fillWidth: true
                            visible: !root.isFileMode && !root.isVolumeMode
                            Text {
                                //% "Preserve disk signature"
                                text: qsTrId("aegra.restore.preserve_signature")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                Layout.preferredWidth: 200
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            Text {
                                text: root.optionOnOff(root.preserveSignature)
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: !root.isFileMode && !root.isVolumeMode
                            Text {
                                //% "Auto-extend last partition"
                                text: qsTrId("aegra.restore.auto_extend")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                Layout.preferredWidth: 200
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            Text {
                                text: root.optionOnOff(root.autoExtend)
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                            }
                        }

                        // Volume mode: no extra disk options — show a short note
                        Text {
                            Layout.fillWidth: true
                            visible: root.isVolumeMode
                            //% "Volume restore uses the mapped target volume; partition layout is not rewritten."
                            text: qsTrId("aegra.restore.summary.volume_options_note")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }

                        // File options
                        RowLayout {
                            Layout.fillWidth: true
                            visible: root.isFileMode
                            Text {
                                //% "Restore security (ACL)"
                                text: qsTrId("aegra.restore.file.restore_security")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            Text {
                                text: root.optionOnOff(root.fileRestoreSecurity)
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: root.isFileMode
                            Text {
                                //% "If a file already exists"
                                text: qsTrId("aegra.restore.file.conflict_policy")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                Layout.preferredWidth: 160
                            }
                            Text {
                                text: root.fileConflictPolicyLabel
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: 12
                    color: Theme.colorCard
                    border.width: 1
                    border.color: Theme.colorBorder
                    implicitHeight: summaryProgressCol.implicitHeight + 32

                    ColumnLayout {
                        id: summaryProgressCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 16
                        spacing: 12

                        Text {
                            Layout.fillWidth: true
                            text: root.restoreProgressLabel
                            color: root.restoreProgressFailed
                                   ? Theme.colorAccentRed
                                   : (root.restoreProgressSucceeded
                                      ? Theme.colorGreen : Theme.colorTextWhite)
                            font.pixelSize: 14
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }

                        TaskProgressBar {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 10
                            value: root.restoreProgressFailed
                                   ? Math.max(root.restoreProgressPercent, 8)
                                   : (root.restoreProgressSucceeded
                                      ? 100 : root.restoreProgressPercent)
                            active: root.restoreProgressActive
                                    || root.restoreProgressFailed
                                    || root.restoreProgressSucceeded
                            fillColor: root.restoreProgressFillColor
                        }

                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            visible: root.restoreSessionStarted && !root.restoreProgressFailed
                            text: (root.restoreProgressSucceeded
                                   ? 100 : root.restoreProgressPercent) + "%"
                            color: root.restoreProgressSucceeded
                                   ? Theme.colorGreen : Theme.colorTextGrey
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: root.restoreProgressFailed
                                     && root.restoreProgressErrorText.length > 0
                            text: root.restoreProgressErrorText
                            color: Theme.colorAccentRed
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
            } // Flickable
        } // restoreStep2
        } // restoreStepContainer

        // Footer: Back + Next on workspace; Back + Restore on summary step.
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            visible: root.onWorkspaceStep
                     || (root.onSummaryStep && !root.restoreSessionRunning)
            spacing: 12
            Item { Layout.fillWidth: true }
            AppButton {
                id: backButton
                Layout.preferredWidth: 100
                Layout.preferredHeight: 40
                // After successful restore only Done remains (no Back).
                visible: !(root.onSummaryStep && root.restoreProgressSucceeded)
                //% "Back"
                text: qsTrId("aegra.common.back")
                enabled: !root.restoreSessionRunning
                         && !root.restoreProgressSucceeded
                         && !serviceClient.restoreCommandBusy
                         && !root.filePreflightPending
                onClicked: root.stepBarBack()
            }
            AppButton {
                id: nextButton
                Layout.preferredWidth: 140
                Layout.preferredHeight: 40
                visible: root.onWorkspaceStep
                text: {
                    if (root.filePreflightPending || serviceClient.restoreCommandBusy)
                        //% "Checking..."
                        return qsTrId("aegra.restore.summary.checking")
                    //% "Next"
                    return qsTrId("aegra.common.next")
                }
                primary: true
                enabled: !root.filePreflightPending && !serviceClient.restoreCommandBusy
                onClicked: {
                    if (!root.canRestore) {
                        var reason = root.restoreBlockReason
                        serviceClient.showToast(
                            reason.length > 0 ? reason : qsTrId("aegra.restore.cannot_restore"),
                            true)
                        return
                    }
                    root.goToSummary()
                }
            }
            AppButton {
                id: restoreButton
                Layout.preferredWidth: 140
                Layout.preferredHeight: 40
                visible: root.onSummaryStep
                         && !root.restoreSessionComplete
                         && !root.restoreJobsSubmitted
                         && !root.restoreSessionRunning
                text: {
                    if (serviceClient.restoreCommandBusy)
                        //% "Restoring..."
                        return qsTrId("aegra.restore.restoring")
                    //% "Restore"
                    return qsTrId("aegra.nav.restore")
                }
                primary: true
                enabled: root.canRestore && !serviceClient.restoreCommandBusy
                ToolTip.delay: 400
                ToolTip.visible: restoreButton.hovered && !root.canRestore
                                 && root.restoreBlockReason.length > 0
                ToolTip.text: root.restoreBlockReason
                onClicked: root.startMappedRestore()
            }
            AppButton {
                id: summaryDoneButton
                Layout.preferredWidth: 140
                Layout.preferredHeight: 40
                visible: root.onSummaryStep && root.restoreSessionComplete
                //% "Done"
                text: qsTrId("aegra.restore.summary.done")
                primary: true
                onClicked: root.goBackToTypeSelection()
            }
        }
    }

    CheckpointCalendarPanel {
        anchors.fill: parent
        z: 2000
        open: root.checkpointPanelOpen
        backupDates: root.backupDates
        checkpoints: root.panelCheckpoints
        checkpointsEpoch: root.panelCheckpointsEpoch
        selectedDate: root.panelSelectedDate
        loading: serviceClient.repositoryLoading
        onClosed: root.checkpointPanelOpen = false
        onDateSelected: function(dateStr) {
            root.onPanelDateSelected(dateStr)
        }
        onCheckpointSelected: function(item) {
            root.applySelectedCheckpoint(item)
            root.checkpointPanelOpen = false
        }
    }
}
