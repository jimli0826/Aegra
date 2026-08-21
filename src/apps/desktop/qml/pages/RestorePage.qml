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
    /// NTFS shrink analyze confirmation (volume mode, capability-gated).
    property bool shrinkConfirmOpen: false
    property var shrinkConfirmDetails: ({})
    /// Smaller-target mapping waiting for exact, target-bound analysis.
    property var pendingShrinkMapping: null
    /// Target-bound shrink analyses, keyed by source volume index.
    property var analyzedShrinkMappings: ({})
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
    /// target disk_number → manual layout edit { sourceDisk, targetBytes, segments: [...] }.
    /// Size changes are only via Target partition-bar edge drag (no auto-expand option).
    property var targetLayoutEdits: ({})
    /// Bumped when a mapped target partition edge is resized.
    property int layoutEditEpoch: 0
    /// True while a source-disk drag hovers a target that fails restore checks.
    property bool mappingDropBlocked: false
    /// Localized block reason shown after the ban icon on the drag ghost.
    property string mappingDropBlockReason: ""
    /// True while a source disk/volume is being dragged — freezes list scrolling.
    property bool mappingDragActive: false
    /// Avoid double inventory refresh after one restore session completes.
    property bool restoreTargetsRefreshed: false
    /// True while resizing a mapped-target partition edge (freeze target ListView).
    property bool layoutResizeActive: false

    function setMappingDropFeedback(blocked, reason) {
        root.mappingDropBlocked = blocked
        root.mappingDropBlockReason = blocked ? (reason || "") : ""
    }

    function clearMappingDropBlocked() {
        root.mappingDropBlocked = false
        root.mappingDropBlockReason = ""
    }

    function beginMappingDrag() {
        root.mappingDragActive = true
        root.clearMappingDropBlocked()
    }

    function endMappingDrag() {
        root.mappingDragActive = false
        root.clearMappingDropBlocked()
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
        root.shrinkConfirmOpen = false
        root.shrinkConfirmDetails = ({})
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
        var shrink = root.readyMappedShrinkAnalysis()
        if (shrink) {
            root.shrinkConfirmDetails = shrink.details || ({})
            root.shrinkConfirmOpen = true
        }
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
               && (!root.isVolumeMode || !root.needsNtfsShrinkAnalyze)
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
                   //% "Drag a source volume onto a target volume"
                   ? qsTrId("aegra.restore.volume_map_required")
                   //% "Drag a source disk onto a target disk"
                   : qsTrId("aegra.restore.map_required")
        if (root.isVolumeMode && root.needsNtfsShrinkAnalyze)
            return qsTrId("aegra.restore.volume_map_required")
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
        root.targetLayoutEdits = ({})
        root.layoutEditEpoch++
        root.mappingEpoch++
    }

    function clearTargetLayoutEdit(targetNum) {
        if (targetNum === undefined || targetNum === null || Number(targetNum) < 0)
            return
        var key = String(targetNum)
        if (!root.targetLayoutEdits || root.targetLayoutEdits[key] === undefined)
            return
        var next = Object.assign({}, root.targetLayoutEdits)
        delete next[key]
        root.targetLayoutEdits = next
        root.layoutEditEpoch++
    }

    function volumeSupportsEnlarge(seg) {
        if (!seg || seg.unallocated === true || seg.notBackedUp === true
                || seg.reserved === true)
            return false
        var fs = String(seg.fileSystem || seg.fs || "").toUpperCase()
        // Worker online-extend supports NTFS/ReFS; FAT/exFAT cannot grow.
        if (fs.indexOf("FAT") >= 0 || fs.indexOf("EXFAT") >= 0)
            return false
        return true
    }

    /// Fully consumed Unallocated kept so the adjacent volume grip stays hittable.
    function isPlaceholderUnallocated(seg) {
        return !!(seg && seg.unallocated === true
                  && (Number(seg.totalBytes) || 0) <= 0)
    }

    function placeholderUnallocatedSegment() {
        return {
            letter: "",
            //% "Unallocated"
            name: qsTrId("aegra.restore.unallocated"),
            size: "",
            fileSystem: "",
            totalBytes: 0,
            freeBytes: -1,
            offsetBytes: -1,
            sourceOffsetBytes: -1,
            unallocated: true,
            notBackedUp: false,
            reserved: false,
            ratio: 0
        }
    }

    /// Insert a 0-byte Unallocated chip when shrinking at a disk edge with no neighbor.
    /// Returns { index, neighIdx } or null if that edge cannot receive free space.
    function attachUnallocatedForShrink(segs, index, edge) {
        var neighIdx = edge === "left" ? index - 1 : index + 1
        if (neighIdx >= 0 && neighIdx < segs.length && segs[neighIdx]
                && segs[neighIdx].unallocated === true
                && segs[neighIdx].reserved !== true)
            return { index: index, neighIdx: neighIdx }
        if (edge === "left" && index === 0) {
            segs.splice(0, 0, root.placeholderUnallocatedSegment())
            return { index: 1, neighIdx: 0 }
        }
        if (edge === "right" && index === segs.length - 1) {
            segs.push(root.placeholderUnallocatedSegment())
            return { index: index, neighIdx: segs.length - 1 }
        }
        return null
    }

    function annotateResizableSegments(segments) {
        var out = []
        var n = segments ? segments.length : 0
        for (var i = 0; i < n; ++i) {
            var s = Object.assign({}, segments[i])
            // Reserved (EFI/MSR/Recovery) are fixed anchors — never resizable.
            if (s.reserved === true) {
                s.canResizeLeft = false
                s.canResizeRight = false
                s.resizable = false
                s.minBytes = Number(s.totalBytes) || 0
                out.push(s)
                continue
            }
            if (s.minBytes === undefined || s.minBytes === null)
                s.minBytes = Number(s.totalBytes) || 0
            var enlarge = root.volumeSupportsEnlarge(s)
            var leftUn = i > 0 && segments[i - 1] && segments[i - 1].unallocated === true
            var rightUn = i + 1 < n && segments[i + 1] && segments[i + 1].unallocated === true
            var canShrink = enlarge && (Number(s.totalBytes) || 0) > (Number(s.minBytes) || 0)
            // 0-byte Unallocated still counts as a neighbor. If it was dropped and the
            // volume now fills the disk, both edges stay grippable so the user can pull back.
            var fillsDisk = canShrink && n === 1
            s.canResizeLeft = enlarge && (leftUn || (fillsDisk && i === 0))
            s.canResizeRight = enlarge && (rightUn || (fillsDisk && i === 0))
            s.resizable = s.canResizeLeft || s.canResizeRight
            out.push(s)
        }
        return out
    }

    function finalizePreviewRatios(segments, targetTotal) {
        var total = 0
        for (var t = 0; t < segments.length; ++t)
            total += Number(segments[t].totalBytes) || 0
        var basis = targetTotal > 0 ? targetTotal : total
        for (var r = 0; r < segments.length; ++r) {
            var tb = Number(segments[r].totalBytes) || 0
            var ratio = basis > 0 ? (tb / basis)
                                  : (1.0 / Math.max(1, segments.length))
            // 0-byte Unallocated must not steal a 4% floor (would hide the volume grip).
            if (tb <= 0)
                segments[r].ratio = 0
            else
                segments[r].ratio = ratio > 0 ? ratio : 0.04
            segments[r].size = root.formatBarBytes(tb)
        }
        return root.annotateResizableSegments(segments)
    }

    /// PhysicalDrive / GPT: 512-byte sector. UI resize snaps to 1 MiB (also sector-aligned).
    readonly property int layoutSectorBytes: 512
    readonly property int layoutStepBytes: 1024 * 1024

    function alignDownBytes(n, step) {
        var s = step > 0 ? step : root.layoutSectorBytes
        n = Number(n) || 0
        if (n <= 0)
            return 0
        return Math.floor(n / s) * s
    }

    function alignRoundBytes(n, step) {
        var s = step > 0 ? step : root.layoutStepBytes
        n = Number(n) || 0
        if (n <= 0)
            return 0
        return Math.round(n / s) * s
    }

    /// Grow/shrink a data volume at `index` into an adjacent Unallocated neighbor.
    /// edge: "left" | "right". deltaBytes > 0 grows the volume toward that edge.
    /// Sizes snap to whole MiB; bar labels use LocaleFormat (GB: two decimals).
    /// Returns the (possibly shifted) volume index after apply, or -1 if unchanged.
    function resizeMappedTargetSegment(targetNum, index, edge, deltaBytes) {
        var key = String(targetNum)
        var edit = root.targetLayoutEdits ? root.targetLayoutEdits[key] : null
        if (!edit || !edit.segments || index < 0 || index >= edit.segments.length)
            return -1
        var segs = []
        for (var c = 0; c < edit.segments.length; ++c)
            segs.push(Object.assign({}, edit.segments[c]))

        var vol = segs[index]
        if (!vol || vol.unallocated || vol.reserved === true || vol.notBackedUp === true)
            return -1
        var step = root.layoutStepBytes
        var minB = Number(vol.minBytes) || 0
        var cur = Number(vol.totalBytes) || 0
        var neighIdx = edge === "left" ? index - 1 : index + 1
        var neigh = (neighIdx >= 0 && neighIdx < segs.length) ? segs[neighIdx] : null
        if (!neigh || neigh.unallocated !== true || neigh.reserved === true) {
            if (Number(deltaBytes) >= 0)
                return -1
            var attached = root.attachUnallocatedForShrink(segs, index, edge)
            if (!attached)
                return -1
            index = attached.index
            neighIdx = attached.neighIdx
            vol = segs[index]
            neigh = segs[neighIdx]
        }
        if (edge === "left" && !vol.canResizeLeft
                && !(root.volumeSupportsEnlarge(vol) && neigh && neigh.unallocated))
            return -1
        if (edge === "right" && !vol.canResizeRight
                && !(root.volumeSupportsEnlarge(vol) && neigh && neigh.unallocated))
            return -1

        var maxGrow = Number(neigh.totalBytes) || 0
        var maxSize = cur + maxGrow
        // Snap to 1 MiB; ignore sub-MiB drag noise.
        var want = root.alignRoundBytes(cur + Number(deltaBytes), step)
        if (want < minB)
            want = minB
        if (want > maxSize)
            want = root.alignDownBytes(maxSize, step)
        if (want < minB)
            want = minB
        // Require at least one full MiB change when leaving min size (or any change).
        if (want !== minB && want !== cur) {
            var diff = Math.abs(want - cur)
            if (diff > 0 && diff < step) {
                if (Number(deltaBytes) > 0)
                    want = Math.min(maxSize, cur + step)
                else
                    want = Math.max(minB, cur - step)
                want = root.alignRoundBytes(want, step)
                if (want < minB)
                    want = minB
                if (want > maxSize)
                    want = root.alignDownBytes(maxSize, step)
            }
        }
        var actual = want - cur
        if (actual === 0)
            return index
        // Keep source used bytes fixed; growth/shrink only changes free space in the preview.
        // Otherwise usedRatio = (total−free)/total climbs as the chip widens and the fill
        // looks like "used data grew" when the user only enlarged the partition.
        var freeB = Number(vol.freeBytes)
        if (!isNaN(freeB) && freeB >= 0) {
            var usedB = cur - freeB
            if (usedB < 0)
                usedB = 0
            if (usedB > want)
                usedB = want
            vol.freeBytes = want - usedB
        }
        vol.totalBytes = want
        neigh.totalBytes = maxGrow - actual
        // Snap neighbor to whole MiB when possible; keep residual on trailing free.
        if (neigh.totalBytes > 0)
            neigh.totalBytes = root.alignDownBytes(neigh.totalBytes, step)
        else
            neigh.totalBytes = 0
        // Keep a 0-byte Unallocated neighbor so the grip stays after a full grow.
        var origin = (edit.layoutOriginBytes !== undefined) ? edit.layoutOriginBytes : null
        segs = root.recomputeSegmentOffsets(segs, origin)
        var targetTotal = Number(edit.targetBytes) || 0
        segs = root.finalizePreviewRatios(segs, targetTotal)
        segs = root.recomputeSegmentOffsets(segs, origin)
        var next = Object.assign({}, root.targetLayoutEdits)
        next[key] = {
            sourceDisk: edit.sourceDisk,
            targetBytes: edit.targetBytes,
            layoutOriginBytes: root.layoutOriginBytes(segs, origin),
            segments: segs
        }
        root.targetLayoutEdits = next
        root.layoutEditEpoch++
        return index
    }

    function clearVolumeMappings() {
        root.volumeMappings = ({})
        root.pendingShrinkMapping = null
        root.analyzedShrinkMappings = ({})
        root.shrinkConfirmOpen = false
        root.shrinkConfirmDetails = ({})
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
        return root.sourceDiskNumberMappedToTarget(diskNum) >= 0
    }

    /// Source disk_number mapped onto this target, or -1 when none.
    function sourceDiskNumberMappedToTarget(targetNum) {
        var _e = root.mappingEpoch
        if (targetNum === undefined || targetNum === null || Number(targetNum) < 0)
            return -1
        var map = root.diskMappings || {}
        for (var k in map) {
            if (Number(map[k]) === Number(targetNum))
                return Number(k)
        }
        return -1
    }

    function sourceDiskDataByNumber(diskNum) {
        var sources = root.sourceDisks || []
        for (var i = 0; i < sources.length; ++i) {
            if (Number(sources[i].diskNumber) === Number(diskNum))
                return sources[i]
        }
        return null
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
        // Archive (.bkf) on the target PhysicalDrive — Worker would reject after layout delete.
        var hostDisk = serviceClient.defaultRepositoryHostDiskNumber()
        if (hostDisk >= 0 && Number(targetNum) === Number(hostDisk))
            //% "Backup archive is on this disk; choose another target"
            return qsTrId("aegra.restore.archive_on_target_disk")
        return ""
    }

    function volumeMappingBlockReason(sourceVolumeIndex, targetSourceId) {
        if (sourceVolumeIndex < 0 || !targetSourceId || targetSourceId.length === 0)
            return ""
        // Capability-off rejects smaller targets; capability-on analyzes before committing a map.
        if (!serviceClient.ntfsShrinkAvailable
                && !root.targetVolumeLargeEnough(sourceVolumeIndex, targetSourceId))
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
        var hostVol = serviceClient.defaultRepositoryHostVolumeSourceId()
        if (hostVol && hostVol.length > 0 && String(targetSourceId) === String(hostVol))
            //% "Backup archive is on this volume; choose another target"
            return qsTrId("aegra.restore.archive_on_target_volume")
        return ""
    }

    function setDiskMapping(sourceNum, targetNum) {
        if (sourceNum < 0)
            return
        // Invalid targets are rejected silently; drag ghost already shows the reason.
        if (targetNum >= 0 && root.mappingBlockReason(sourceNum, targetNum).length > 0)
            return
        var prev = root.mappedTarget(sourceNum)
        if (prev >= 0 && prev !== Number(targetNum))
            root.clearTargetLayoutEdit(prev)
        if (targetNum >= 0)
            root.clearTargetLayoutEdit(targetNum)
        var map = Object.assign({}, root.diskMappings || {})
        map[String(sourceNum)] = targetNum
        root.diskMappings = map
        root.mappingEpoch++
    }

    function commitVolumeMapping(sourceVolumeIndex, targetSourceId) {
        var analyses = Object.assign({}, root.analyzedShrinkMappings || {})
        delete analyses[String(sourceVolumeIndex)]
        root.analyzedShrinkMappings = analyses
        var map = Object.assign({}, root.volumeMappings || {})
        map[String(sourceVolumeIndex)] = targetSourceId || ""
        root.volumeMappings = map
        root.mappingEpoch++
    }

    function beginShrinkMappingAnalysis(sourceVolumeIndex, targetSourceId) {
        root.pendingShrinkMapping = {
            sourceVolumeIndex: Number(sourceVolumeIndex),
            targetSourceId: String(targetSourceId)
        }
        root.shrinkConfirmOpen = false
        root.shrinkConfirmDetails = ({})
        var started = serviceClient.analyzeVolumeShrink(
                    sourceVolumeIndex, targetSourceId,
                    root.selectedCheckpointId, root.pendingLayoutPassword)
        if (!started)
            root.pendingShrinkMapping = null
        return started
    }

    function setVolumeMapping(sourceVolumeIndex, targetSourceId) {
        if (sourceVolumeIndex < 0)
            return
        var tid = targetSourceId || ""
        if (tid.length > 0
                && root.volumeMappingBlockReason(sourceVolumeIndex, tid).length > 0)
            return
        if (tid.length > 0
                && !root.targetVolumeLargeEnough(sourceVolumeIndex, tid)) {
            root.beginShrinkMappingAnalysis(sourceVolumeIndex, tid)
            return
        }
        root.commitVolumeMapping(sourceVolumeIndex, tid)
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
        var hostDisk = serviceClient.defaultRepositoryHostDiskNumber()
        var targets = root.targetDisks || []
        for (var i = 0; i < targets.length; ++i) {
            var d = targets[i]
            var num = Number(d.diskNumber)
            var lab = d.name || ("Disk " + num)
            if (d.size)
                lab += "  (" + d.size + ")"
            var isSystem = d.isSystemDisk === true
            var hasArchive = hostDisk >= 0 && Number(num) === Number(hostDisk)
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
            else if (hasArchive)
                //% "— archive here"
                lab += "  " + qsTrId("aegra.restore.archive_here_tag")
            out.push({
                label: lab,
                value: num,
                tooSmall: tooSmall,
                inUse: inUse,
                isSystem: isSystem,
                hasArchive: hasArchive,
                enabled: !tooSmall && !inUse && !isSystem && !hasArchive
            })
        }
        return out
    }

    /// Default every source unmapped; user maps by dragging onto a target.
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
        var hostVol = serviceClient.defaultRepositoryHostVolumeSourceId() || ""
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
            var hasArchive = hostVol.length > 0 && String(sid) === String(hostVol)
            if (isSystem)
                //% "[System]"
                lab += "  " + qsTrId("aegra.restore.system_tag")
            var tooSmall = !root.targetVolumeLargeEnough(sourceVolumeIndex, sid)
            var shrinkOk = tooSmall && serviceClient.ntfsShrinkAvailable
            var inUse = root.isVolumeTargetMappedByOther(sourceVolumeIndex, sid)
            if (tooSmall && !shrinkOk)
                //% "— too small"
                lab += "  " + qsTrId("aegra.restore.target_too_small_tag")
            else if (shrinkOk)
                //% "— shrink"
                lab += "  " + qsTrId("aegra.restore.shrink_candidate_tag")
            else if (inUse)
                //% "— in use"
                lab += "  " + qsTrId("aegra.restore.target_in_use_tag")
            else if (isSystem)
                //% "— PE only"
                lab += "  " + qsTrId("aegra.restore.pe_only_tag")
            else if (hasArchive)
                //% "— archive here"
                lab += "  " + qsTrId("aegra.restore.archive_here_tag")
            else if (isReadOnly)
                //% "— read-only"
                lab += "  " + qsTrId("aegra.restore.read_only_tag")
            out.push({
                label: lab,
                value: sid,
                tooSmall: tooSmall,
                shrinkCandidate: shrinkOk,
                inUse: inUse,
                isSystem: isSystem,
                isReadOnly: isReadOnly,
                hasArchive: hasArchive,
                enabled: (!tooSmall || shrinkOk) && !inUse && !isSystem && !isReadOnly && !hasArchive
            })
        }
        return out
    }

    function shrinkAnalysisMatches(pair) {
        var analyzed = root.shrinkAnalysisFor(pair)
        return !!analyzed
    }

    function shrinkAnalysisFor(pair) {
        if (!pair)
            return null
        var analyses = root.analyzedShrinkMappings || ({})
        var analyzed = analyses[String(pair.sourceVolumeIndex)]
        return !!(analyzed && pair
                   && Number(analyzed.sourceVolumeIndex) === Number(pair.sourceVolumeIndex)
                   && String(analyzed.targetSourceId) === String(pair.targetSourceId))
                ? analyzed : null
    }

    function readyMappedShrinkAnalysis() {
        var pair = root.firstShrinkVolumePair()
        return root.shrinkAnalysisFor(pair)
    }

    /// True only for a stale smaller mapping that has no target-bound analysis token.
    readonly property bool needsNtfsShrinkAnalyze: {
        var _e = root.mappingEpoch
        if (!root.isVolumeMode || !serviceClient.ntfsShrinkAvailable)
            return false
        var pairs = root.allMappedVolumePairs()
        for (var i = 0; i < pairs.length; ++i) {
            if (!root.targetVolumeLargeEnough(pairs[i].sourceVolumeIndex, pairs[i].targetSourceId)
                    && !root.shrinkAnalysisMatches(pairs[i]))
                return true
        }
        return false
    }

    function firstShrinkVolumePair() {
        var pairs = root.allMappedVolumePairs()
        for (var i = 0; i < pairs.length; ++i) {
            if (!root.targetVolumeLargeEnough(pairs[i].sourceVolumeIndex, pairs[i].targetSourceId))
                return pairs[i]
        }
        return null
    }

    function shrinkCapacityText(details) {
        var target = serviceClient.formatBytes(Number(details.targetCapacityBytes) || 0)
        var minimum = serviceClient.formatBytes(Number(details.minimumTargetBytes) || 0)
        return qsTrId("aegra.restore.shrink_minimum_target").arg(minimum)
                + "  ·  " + qsTrId("aegra.restore.shrink_target_size").arg(target)
    }

    function acceptAnalyzedShrinkMapping(details) {
        var pending = root.pendingShrinkMapping
        root.pendingShrinkMapping = null
        if (!pending)
            return
        if (String(details.targetSourceId || "") !== String(pending.targetSourceId)
                || String(details.recoveryPointId || "") !== root.selectedCheckpointId)
            return
        var targetBytes = Number(details.targetCapacityBytes) || 0
        var minimumBytes = Number(details.minimumTargetBytes) || 0
        if (minimumBytes <= 0 || targetBytes < minimumBytes) {
            serviceClient.showToast(root.shrinkCapacityText(details), true)
            return
        }
        root.commitVolumeMapping(pending.sourceVolumeIndex, pending.targetSourceId)
        var analyses = Object.assign({}, root.analyzedShrinkMappings || {})
        analyses[String(pending.sourceVolumeIndex)] = {
            sourceVolumeIndex: pending.sourceVolumeIndex,
            targetSourceId: pending.targetSourceId,
            details: details || ({})
        }
        root.analyzedShrinkMappings = analyses
        serviceClient.showToast(root.shrinkCapacityText(details), false)
        if (root.restoreStep === 2) {
            root.shrinkConfirmDetails = details || ({})
            root.shrinkConfirmOpen = true
        }
    }

    function rejectAnalyzedShrinkMapping() {
        root.pendingShrinkMapping = null
        root.shrinkConfirmOpen = false
        root.shrinkConfirmDetails = ({})
    }

    function confirmShrinkRestore() {
        root.shrinkConfirmOpen = false
        root.beginMappedRestoreQueue()
    }

    function cancelShrinkConfirm() {
        root.shrinkConfirmOpen = false
        root.shrinkConfirmDetails = ({})
    }

    function beginRestoreSession() {
        root.restoreSessionStartMs = Date.now()
        root.restoreJobsSubmitted = false
        root.restoreSessionFailed = false
        root.restoreSessionErrorText = ""
        root.filePreflightPending = false
        root.restoreTargetsRefreshed = false
        if (root.restoreStep !== 2)
                root.restoreStep = 2
    }

    /// After disk/volume restore finishes, drop mapping preview and re-query live inventory
    /// so Target Disks match Disk Management (not the pre-restore layout / edit overlay).
    function refreshTargetsAfterRestore() {
        if (root.restoreTargetsRefreshed)
            return
        root.restoreTargetsRefreshed = true
        root.clearDiskMappings()
        root.clearVolumeMappings()
        if (typeof serviceClient !== "undefined" && serviceClient
                && typeof serviceClient.refreshInventory === "function")
            serviceClient.refreshInventory()
    }

    onRestoreSessionCompleteChanged: {
        if (!root.restoreSessionComplete || root.restoreTargetsRefreshed)
            return
        // Brief delay: volumes may still be mounting after Worker bring_online.
        restoreTargetRefreshTimer.restart()
    }

    Timer {
        id: restoreTargetRefreshTimer
        interval: 1200
        repeat: false
        onTriggered: root.refreshTargetsAfterRestore()
    }

    /// Absolute byte offset of the first visible chip on the physical disk.
    /// Must NOT use a data volume's sourceOffset alone (that is the volume start, not
    /// the bar origin — using it shifted H right and left free became ~1.5GB).
    /// Prefer: hint → first segment absolute offset → firstData.sourceOffset − preceding sizes.
    function layoutOriginBytes(segments, hintOrigin) {
        if (hintOrigin !== undefined && hintOrigin !== null && Number(hintOrigin) >= 0)
            return Number(hintOrigin)
        if (!segments || segments.length === 0)
            return 0
        // First chip already has an absolute offset (e.g. leading unalloc after GPT).
        var firstOff = Number(segments[0] && segments[0].offsetBytes)
        if (!isNaN(firstOff) && firstOff >= 0)
            return firstOff
        // Infer from first data volume's source start minus sizes before it.
        var before = 0
        for (var i = 0; i < segments.length; ++i) {
            var s = segments[i]
            if (!s)
                continue
            if (s.unallocated !== true && s.notBackedUp !== true) {
                var src = Number(s.sourceOffsetBytes)
                if (!isNaN(src) && src >= 0) {
                    var origin = src - before
                    return origin > 0 ? origin : 0
                }
            }
            before += Number(s.totalBytes) || 0
        }
        return 0
    }

    /// Recompute absolute offsets from sizes, preserving disk origin (not bar-relative 0).
    function recomputeSegmentOffsets(segments, hintOrigin) {
        var cursor = root.layoutOriginBytes(segments, hintOrigin)
        for (var i = 0; i < segments.length; ++i) {
            if (!segments[i])
                continue
            segments[i].offsetBytes = cursor
            cursor += Number(segments[i].totalBytes) || 0
        }
        return segments
    }

    /// Target-bar layout for disk restore: source start → target start + size (left-drag OK).
    function partitionLayoutEditsForTarget(targetNum) {
        var key = String(targetNum)
        var edit = root.targetLayoutEdits ? root.targetLayoutEdits[key] : null
        if (!edit || !edit.segments || edit.segments.length === 0)
            return []
        var origin = (edit.layoutOriginBytes !== undefined) ? edit.layoutOriginBytes : null
        var segs = root.recomputeSegmentOffsets(edit.segments.slice(), origin)
        // Usable end: leave 1 MiB for GPT backup header region (matches Worker clamp).
        var diskCap = Number(edit.targetBytes) || 0
        var maxEnd = diskCap
        if (diskCap > root.layoutStepBytes)
            maxEnd = root.alignDownBytes(diskCap - root.layoutStepBytes, root.layoutSectorBytes)
        var out = []
        for (var i = 0; i < segs.length; ++i) {
            var s = segs[i]
            // Reserved/unallocated are anchors only — Worker resolves final table.
            if (!s || s.unallocated === true || s.notBackedUp === true
                    || s.reserved === true)
                continue
            var srcOff = Number(s.sourceOffsetBytes)
            var tgtOff = Number(s.offsetBytes)
            var size = Number(s.totalBytes) || 0
            var minB = Number(s.minBytes) || 0
            if (isNaN(srcOff) || srcOff < 0 || isNaN(tgtOff) || tgtOff < 0 || size <= 0)
                continue
            // Sector-align; Worker still re-clamps against reserved ranges.
            var sector = root.layoutSectorBytes
            srcOff = root.alignDownBytes(srcOff, sector)
            tgtOff = root.alignDownBytes(tgtOff, sector)
            size = root.alignDownBytes(size, sector)
            if (size < minB)
                size = root.alignDownBytes(minB, sector)
            if (maxEnd > 0 && tgtOff + size > maxEnd) {
                if (tgtOff >= maxEnd)
                    continue
                size = root.alignDownBytes(maxEnd - tgtOff, sector)
            }
            if (size <= 0)
                continue
            // Hints only: Worker places data partitions without overlapping reserved.
            out.push({
                sourceStartOffsetBytes: srcOff,
                targetStartOffsetBytes: tgtOff,
                sizeBytes: size
            })
        }
        return out
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
            var shrink = root.targetVolumeLargeEnough(pair.sourceVolumeIndex,
                                                       pair.targetSourceId)
                    ? null : root.shrinkAnalysisFor(pair)
            if (shrink) {
                var token = String((shrink.details || ({})).preflightToken || "")
                ok = serviceClient.startRestoreWithPreflightToken(
                            token, root.pendingLayoutPassword)
            } else {
                ok = serviceClient.startVolumeRestore(pair.sourceVolumeIndex,
                                                      pair.targetSourceId,
                                                      root.selectedCheckpointId,
                                                      root.pendingLayoutPassword)
            }
        } else {
            // auto_expand_last_partition always false: grow only via Target bar edge drag.
            ok = serviceClient.startDiskRestore(
                    pair.source, pair.target,
                    root.selectedCheckpointId,
                    root.pendingLayoutPassword,
                    root.preserveSignature,
                    false,
                    root.partitionLayoutEditsForTarget(pair.target))
        }
        if (!ok) {
            root.pendingRestoreQueue = []
            root.multiRestoreActive = false
            root.restoreJobsSubmitted = true
            root.restoreSessionFailed = true
        }
    }

    function beginMappedRestoreQueue() {
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
                //% "Drag a source volume onto a target volume"
                ? qsTrId("aegra.restore.volume_map_required")
                //% "Drag a source disk onto a target disk"
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

    function startMappedRestore() {
        // Summary step owns the Restore action; workspace only advances via Next.
        if (root.restoreStep !== 2)
            return
        if (!root.canRestore)
            return
        if (root.restoreJobsSubmitted && !root.restoreSessionFailed && !root.restoreSessionComplete)
            return
        var analyzedShrink = root.isVolumeMode ? root.readyMappedShrinkAnalysis() : null
        if (analyzedShrink) {
            root.shrinkConfirmDetails = analyzedShrink.details || ({})
            root.shrinkConfirmOpen = true
            return
        }
        root.beginMappedRestoreQueue()
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
        function onNtfsShrinkAnalyzeSucceeded(details) {
            root.acceptAnalyzedShrinkMapping(details || ({}))
        }
        function onNtfsShrinkAnalyzeFailed(message) {
            root.rejectAnalyzedShrinkMapping()
            // Red toast already shown by ServiceClient.
        }
        function onRestorePreflightProvisional(details) {
            // Direct prepare returned provisional — analysis required (capability path).
            root.shrinkConfirmOpen = false
            root.shrinkConfirmDetails = details || ({})
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

    /// Volumes for bar (ratio by capacityBytes).
    /// Prefers offset-ordered segments; free gaps are drawn only when larger than GPT/MSR
    /// padding so the bar matches Windows Disk Management (tiny leading free is omitted).
    /// includeReserved: Target preview keeps EFI/Recovery as fixed chips.
    /// MSR is always omitted on Source and Target bars (geometry advanced, no chip).
    function displayVolumesForDisk(diskData, includeReserved) {
        var keepOtherReserved = includeReserved === true
        var raw = (diskData && diskData.volumes) ? diskData.volumes : []
        var diskTotal = diskData ? (Number(diskData.capacityBytes) || 0) : 0
        var list = []
        // ≤ 128 MiB: alignment, MSR, GPT header/padding — not drawn as Unallocated.
        var minUnallocBytes = 128 * 1024 * 1024
        var hasPrebuiltUnalloc = false
        var hasOffsets = false
        for (var i = 0; i < raw.length; ++i) {
            if (!raw[i])
                continue
            if (raw[i].unallocated === true)
                hasPrebuiltUnalloc = true
            // offsetBytes may be 0 at the start of the disk — presence matters.
            if (raw[i].offsetBytes !== undefined && raw[i].offsetBytes !== null
                    && raw[i].offsetBytes !== "")
                hasOffsets = true
        }

        function isMsrVolume(v) {
            if (!v)
                return false
            if (v.isMsr === true || v.isMsr === 1)
                return true
            var n = ((v.name || "") + " " + (v.title || "") + " " + (v.letter || ""))
                    .toLowerCase().replace(/\s+/g, " ").trim()
            if (n === "msr" || n.indexOf("microsoft reserved") >= 0
                    || n.indexOf("msr partition") >= 0)
                return true
            // Word-boundary MSR (avoids false hits inside longer labels).
            return /(^|[^a-z0-9])msr([^a-z0-9]|$)/.test(n)
        }

        function isReservedVolume(v) {
            if (!v)
                return false
            return v.reserved === true || v.reserved === 1 || isMsrVolume(v)
        }

        /// Always hide MSR. Source also hides nothing else; Target keeps EFI/Recovery.
        function shouldOmitChip(v) {
            if (!v || v.unallocated === true)
                return false
            if (isMsrVolume(v))
                return true
            if (keepOtherReserved)
                return false
            // Source: show EFI/Recovery; only MSR was omitted above.
            return false
        }

        function formatUnallocSize(bytes) {
            var n = Number(bytes) || 0
            if (n <= 0)
                return ""
            if (typeof serviceClient !== "undefined" && serviceClient
                    && typeof serviceClient.formatBytes === "function")
                return serviceClient.formatBytes(n)
            return ""
        }

        function pushUnalloc(bytes, startOffset) {
            if (bytes <= minUnallocBytes)
                return
            var off = (startOffset !== undefined && startOffset !== null
                       && !isNaN(Number(startOffset))) ? Number(startOffset) : -1
            list.push({
                letter: "",
                //% "Unallocated"
                name: qsTrId("aegra.restore.unallocated"),
                size: formatUnallocSize(bytes),
                fileSystem: "",
                totalBytes: bytes,
                offsetBytes: off,
                sourceOffsetBytes: -1,
                unallocated: true,
                reserved: false
            })
        }

        function pushVolume(v, isUnalloc, offsetHint) {
            if (!isUnalloc && shouldOmitChip(v))
                return
            var tb = Number(v.capacityBytes) || 0
            if (isUnalloc && tb <= minUnallocBytes)
                return
            var reserved = !isUnalloc && isReservedVolume(v)
            var notBackedUp = !isUnalloc && !reserved && v.notBackedUp === true
            var sz = v.size || ""
            if ((isUnalloc || notBackedUp) && sz.length === 0)
                sz = formatUnallocSize(tb)
            // freeBytes ≥ 0 when inventory reports free space; -1 = unknown (no used fill).
            var freeB = -1
            if (!isUnalloc && !notBackedUp && !reserved) {
                var rawFree = Number(v.freeBytes)
                if (!isNaN(rawFree) && rawFree >= 0)
                    freeB = rawFree
            }
            var off = -1
            if (offsetHint !== undefined && offsetHint !== null && !isNaN(Number(offsetHint)))
                off = Number(offsetHint)
            else if (v.offsetBytes !== undefined && v.offsetBytes !== null
                     && v.offsetBytes !== "")
                off = Number(v.offsetBytes)
            // sourceOffsetBytes: fixed restore match key (partition start in archive).
            // Reserved keep absolute offset so packing preserves their gap.
            var srcOff = (!isUnalloc && !notBackedUp && off >= 0) ? off : -1
            list.push({
                letter: (isUnalloc || notBackedUp || reserved) ? "" : (v.letter || ""),
                //% "Unallocated"
                name: isUnalloc ? qsTrId("aegra.restore.unallocated")
                      //% "Not backed up"
                      : (notBackedUp ? qsTrId("aegra.restore.not_backed_up")
                                     : (v.name || "")),
                size: sz,
                fileSystem: (isUnalloc || notBackedUp || reserved)
                            ? "" : (v.fs || v.fileSystem || ""),
                totalBytes: tb,
                freeBytes: freeB,
                offsetBytes: off,
                sourceOffsetBytes: srcOff,
                unallocated: isUnalloc,
                notBackedUp: notBackedUp,
                reserved: reserved,
                isMsr: !isUnalloc && isMsrVolume(v)
            })
        }

        if (hasPrebuiltUnalloc) {
            for (var p = 0; p < raw.length; ++p) {
                if (!raw[p])
                    continue
                if (shouldOmitChip(raw[p])) {
                    // Source bar: hide MSR; keep EFI/Recovery chips.
                    continue
                }
                pushVolume(raw[p], raw[p].unallocated === true)
            }
        } else if (hasOffsets) {
            var sorted = []
            for (var s = 0; s < raw.length; ++s) {
                if (raw[s])
                    sorted.push(raw[s])
            }
            sorted.sort(function(a, b) {
                return (Number(a.offsetBytes) || 0) - (Number(b.offsetBytes) || 0)
            })
            var cursor = 0
            for (var k = 0; k < sorted.length; ++k) {
                var vol = sorted[k]
                var off = Number(vol.offsetBytes) || 0
                var cap = Number(vol.capacityBytes) || 0
                var end = off + cap
                if (shouldOmitChip(vol)) {
                    // Occupy MSR space without a chip (Source bar style).
                    if (end > cursor)
                        cursor = end
                    continue
                }
                if (off > cursor)
                    pushUnalloc(off - cursor, cursor)
                pushVolume(vol, false, off)
                if (end > cursor)
                    cursor = end
            }
            if (diskTotal > cursor)
                pushUnalloc(diskTotal - cursor, cursor)
        } else {
            var sumBytes = 0
            for (var j = 0; j < raw.length; ++j) {
                if (!raw[j])
                    continue
                if (shouldOmitChip(raw[j]))
                    continue
                pushVolume(raw[j], false)
                sumBytes += Number(raw[j].capacityBytes) || 0
            }
            if (diskTotal <= 0 && sumBytes > 0)
                diskTotal = sumBytes
            if (diskTotal > 0 && sumBytes > 0)
                pushUnalloc(diskTotal - sumBytes)
        }

        // Ratio over visible segments so a single volume fills the bar when small
        // alignment free space was omitted (same look as Disk Management).
        var sum = 0
        for (var t = 0; t < list.length; ++t)
            sum += list[t].totalBytes || 0
        var basis = sum > 0 ? sum : diskTotal
        for (var r = 0; r < list.length; ++r) {
            var ratio = basis > 0 ? (list[r].totalBytes / basis)
                                  : (1.0 / Math.max(1, list.length))
            list[r].ratio = ratio > 0 ? ratio : 0.04
        }
        return list
    }

    /// Partition-bar labels: same precision as inventory (GB/TB two decimals).
    function formatBarBytes(bytes) {
        var n = Number(bytes) || 0
        if (n <= 0)
            return ""
        if (typeof serviceClient !== "undefined" && serviceClient
                && typeof serviceClient.formatBytes === "function")
            return serviceClient.formatBytes(n)
        return ""
    }

    /// Post-restore preview for a mapped target:
    /// - reserved EFI/Recovery stay as fixed, non-resizable segments (MSR omitted)
    /// - not-backed-up partitions → Unallocated
    /// - when target is larger than source, trailing free is Unallocated
    ///   (grow only by dragging edges on the Target bar; no auto-expand option)
    /// UI sizes/starts are visual hints only; Worker resolves final table.
    function displayVolumesForRestorePreview(sourceDisk, targetDiskData) {
        // Keep EFI/Recovery chips so contiguous packing cannot steal their space.
        // displayVolumesForDisk(..., true) already omits MSR.
        var list = root.displayVolumesForDisk(sourceDisk, true)
        var out = []
        for (var i = 0; i < list.length; ++i) {
            var v = list[i]
            if (!v)
                continue
            if (v.isMsr === true || v.isMsr === 1)
                continue
            var reservedChip = (v.reserved === true || v.reserved === 1)
            if (reservedChip) {
                var rOff = (v.offsetBytes !== undefined && v.offsetBytes !== null)
                           ? Number(v.offsetBytes) : -1
                var rBytes = Number(v.totalBytes) || 0
                out.push({
                    letter: "",
                    name: v.name || "",
                    title: v.title || v.name || "",
                    size: v.size || root.formatBarBytes(rBytes),
                    fileSystem: "",
                    totalBytes: rBytes,
                    freeBytes: -1,
                    offsetBytes: rOff,
                    sourceOffsetBytes: rOff,
                    unallocated: false,
                    notBackedUp: false,
                    reserved: true,
                    isMsr: false,
                    minBytes: rBytes
                })
                continue
            }
            if (v.notBackedUp === true) {
                var bytes = Number(v.totalBytes) || 0
                if (out.length > 0 && out[out.length - 1].unallocated === true) {
                    var prev = out[out.length - 1]
                    prev.totalBytes = (Number(prev.totalBytes) || 0) + bytes
                    prev.size = root.formatBarBytes(prev.totalBytes)
                    continue
                }
                out.push({
                    letter: "",
                    //% "Unallocated"
                    name: qsTrId("aegra.restore.unallocated"),
                    size: root.formatBarBytes(bytes),
                    fileSystem: "",
                    totalBytes: bytes,
                    freeBytes: -1,
                    offsetBytes: -1,
                    sourceOffsetBytes: -1,
                    unallocated: true,
                    notBackedUp: false,
                    reserved: false
                })
                continue
            }
            var srcOff = (v.sourceOffsetBytes !== undefined && v.sourceOffsetBytes !== null)
                         ? Number(v.sourceOffsetBytes)
                         : ((v.offsetBytes !== undefined && v.offsetBytes !== null)
                            ? Number(v.offsetBytes) : -1)
            out.push({
                letter: v.letter || "",
                name: v.name || "",
                title: v.title || "",
                size: v.size || "",
                fileSystem: v.fileSystem || "",
                totalBytes: Number(v.totalBytes) || 0,
                freeBytes: (v.freeBytes !== undefined && v.freeBytes !== null)
                           ? Number(v.freeBytes) : -1,
                offsetBytes: (v.offsetBytes !== undefined && v.offsetBytes !== null)
                             ? Number(v.offsetBytes) : srcOff,
                sourceOffsetBytes: srcOff,
                unallocated: v.unallocated === true,
                notBackedUp: false,
                reserved: false
            })
        }

        var sourceTotal = Number(sourceDisk && sourceDisk.capacityBytes) || 0
        var targetTotal = Number(targetDiskData && targetDiskData.capacityBytes) || 0
        if (targetTotal <= 0)
            targetTotal = sourceTotal

        var sum = 0
        for (var s = 0; s < out.length; ++s)
            sum += Number(out[s].totalBytes) || 0
        // Prefer source disk size as geometry base; fall back to segment sum.
        // With reserved included, undershoot is true free (tiny alignment), not Recovery.
        var baseSize = sourceTotal > 0 ? sourceTotal : sum
        if (baseSize > 0 && sum > 0 && sum < baseSize) {
            var pad = baseSize - sum
            if (out.length > 0 && out[out.length - 1].unallocated === true) {
                out[out.length - 1].totalBytes += pad
                out[out.length - 1].size = root.formatBarBytes(out[out.length - 1].totalBytes)
            } else {
                out.push({
                    letter: "",
                    //% "Unallocated"
                    name: qsTrId("aegra.restore.unallocated"),
                    size: root.formatBarBytes(pad),
                    fileSystem: "",
                    totalBytes: pad,
                    unallocated: true,
                    notBackedUp: false,
                    reserved: false
                })
            }
            sum = baseSize
        }

        // Min size = source geometry; reserved min == size (locked).
        for (var m = 0; m < out.length; ++m) {
            if (out[m].unallocated)
                continue
            var locked = Number(out[m].totalBytes) || 0
            out[m].minBytes = locked
            if (out[m].reserved === true)
                out[m].minBytes = locked
        }

        // Larger target: free space stays Unallocated until the user drags edges.
        var extra = targetTotal > baseSize ? (targetTotal - baseSize) : 0
        if (extra > 0) {
            if (out.length > 0 && out[out.length - 1].unallocated === true) {
                out[out.length - 1].totalBytes =
                    (Number(out[out.length - 1].totalBytes) || 0) + extra
                out[out.length - 1].size =
                    root.formatBarBytes(out[out.length - 1].totalBytes)
            } else {
                out.push({
                    letter: "",
                    //% "Unallocated"
                    name: qsTrId("aegra.restore.unallocated"),
                    size: root.formatBarBytes(extra),
                    fileSystem: "",
                    totalBytes: extra,
                    unallocated: true,
                    notBackedUp: false,
                    reserved: false
                })
            }
        }

        out = root.finalizePreviewRatios(out, targetTotal)
        // Contiguous pack is safe: reserved segments occupy their size in the chain.
        return root.recomputeSegmentOffsets(out, root.layoutOriginBytes(out, null))
    }

    /// Create/refresh mutable layout edit for a mapped target (call from resize press, not bindings).
    function ensureTargetLayoutEdit(sourceDisk, targetDiskData) {
        var tgtNum = targetDiskData && targetDiskData.diskNumber !== undefined
                     ? Number(targetDiskData.diskNumber) : -1
        var srcNum = sourceDisk && sourceDisk.diskNumber !== undefined
                     ? Number(sourceDisk.diskNumber) : -1
        if (tgtNum < 0 || srcNum < 0)
            return null
        var key = String(tgtNum)
        var edit = root.targetLayoutEdits ? root.targetLayoutEdits[key] : null
        if (edit && Number(edit.sourceDisk) === srcNum
                && edit.segments && edit.segments.length > 0)
            return edit
        var base = root.displayVolumesForRestorePreview(sourceDisk, targetDiskData)
        var segs = []
        for (var i = 0; i < base.length; ++i)
            segs.push(Object.assign({}, base[i]))
        var origin = root.layoutOriginBytes(segs, null)
        segs = root.recomputeSegmentOffsets(segs, origin)
        var next = Object.assign({}, root.targetLayoutEdits || {})
        next[key] = {
            sourceDisk: srcNum,
            targetBytes: Number(targetDiskData.capacityBytes) || 0,
            layoutOriginBytes: origin,
            segments: segs
        }
        root.targetLayoutEdits = next
        root.layoutEditEpoch++
        return next[key]
    }

    /// Drop MSR chips from a segment list (Source/Target bars never show them).
    function omitMsrSegments(segments) {
        if (!segments || segments.length === 0)
            return segments || []
        var out = []
        for (var i = 0; i < segments.length; ++i) {
            var s = segments[i]
            if (!s)
                continue
            if (s.isMsr === true || s.isMsr === 1)
                continue
            var n = ((s.name || "") + " " + (s.title || "")).toLowerCase().trim()
            if (n === "msr" || n.indexOf("microsoft reserved") >= 0
                    || n.indexOf("msr partition") >= 0
                    || /(^|[^a-z0-9])msr([^a-z0-9]|$)/.test(n))
                continue
            out.push(s)
        }
        return out
    }

    /// Mapped-target bar layout: default preview, or user-resized edit when still valid.
    function displayVolumesForMappedTarget(sourceDisk, targetDiskData) {
        var tgtNum = targetDiskData && targetDiskData.diskNumber !== undefined
                     ? Number(targetDiskData.diskNumber) : -1
        var srcNum = sourceDisk && sourceDisk.diskNumber !== undefined
                     ? Number(sourceDisk.diskNumber) : -1
        var key = String(tgtNum)
        var edit = root.targetLayoutEdits ? root.targetLayoutEdits[key] : null
        var _le = root.layoutEditEpoch
        if (edit && Number(edit.sourceDisk) === srcNum
                && edit.segments && edit.segments.length > 0) {
            // Stale edits from older builds may still carry an MSR chip — strip it.
            return root.annotateResizableSegments(root.omitMsrSegments(edit.segments))
        }
        return root.displayVolumesForRestorePreview(sourceDisk, targetDiskData)
    }

    /// Pixel widths for partition bar segments: proportional capacity with a floor so
    /// small system partitions (EFI ~100–300 MiB) stay readable. Extra width is taken
    /// from larger segments so the row still fills `rowWidth`.
    function partitionBarWidths(volumes, rowWidth, spacing) {
        var n = volumes ? volumes.length : 0
        if (n <= 0)
            return []
        var gap = Math.max(0, n - 1) * (spacing || 0)
        var avail = Math.max(0, Math.floor(rowWidth) - gap)
        if (avail <= 0) {
            var zeros = []
            for (var z = 0; z < n; ++z)
                zeros.push(0)
            return zeros
        }

        // Named data volumes: ~56px. Letter-less system chips (EFI / Recovery) and
        // hatch chips (Unallocated / Not backed up) need room for full text.
        // Use the same floor for both hatch kinds so Source ("Not backed up") and
        // mapped Target ("Unallocated") keep matching segment widths.
        var minAlloc = Math.min(56, Math.max(20, Math.floor(avail / Math.max(1, n + 1))))
        var minSystem = Math.min(140, Math.max(minAlloc, Math.floor(avail / Math.max(2, n + 1))))
        var minHatch = Math.min(100, Math.max(minAlloc, Math.floor(avail / Math.max(3, n + 2))))

        function isSystemChip(v) {
            if (!v || v.unallocated === true || v.notBackedUp === true)
                return false
            if (v.reserved === true)
                return true
            var letter = (v.letter || "").trim()
            if (letter.length > 0)
                return false
            var n = ((v.name || "") + " " + (v.title || "")).toLowerCase()
            return n.indexOf("efi") >= 0 || n.indexOf("recovery") >= 0
                   || n.indexOf("hidden") >= 0 || n.indexOf("system") >= 0
                   || n.indexOf("msr") >= 0 || n.indexOf("reserved") >= 0
        }

        function isHatchChip(v) {
            return v && (v.unallocated === true || v.notBackedUp === true)
        }

        var mins = []
        var weights = []
        var weightSum = 0
        var minTotal = 0
        var lastDrawn = -1
        for (var i = 0; i < n; ++i) {
            var v = volumes[i]
            if (root.isPlaceholderUnallocated(v)) {
                mins.push(0)
                weights.push(0)
                continue
            }
            var mn = isHatchChip(v) ? minHatch
                     : (isSystemChip(v) ? minSystem : minAlloc)
            mins.push(mn)
            minTotal += mn
            lastDrawn = i
            var w = v && v.ratio > 0 ? Number(v.ratio) : 0
            if (w <= 0)
                w = 1.0 / n
            weights.push(w)
            weightSum += w
        }
        if (weightSum <= 0)
            weightSum = Math.max(1, lastDrawn + 1)

        var widths = []
        if (minTotal >= avail) {
            var scale = minTotal > 0 ? (avail / minTotal) : 0
            var usedScale = 0
            for (var s = 0; s < n; ++s) {
                if (mins[s] <= 0) {
                    widths.push(0)
                    continue
                }
                var sw = (s === lastDrawn) ? (avail - usedScale)
                                           : Math.floor(mins[s] * scale)
                widths.push(Math.max(1, sw))
                usedScale += widths[s]
            }
            return widths
        }

        var rest = avail - minTotal
        var usedExtra = 0
        for (var e = 0; e < n; ++e) {
            if (mins[e] <= 0 && weights[e] <= 0) {
                widths.push(0)
                continue
            }
            var extra = (e === lastDrawn) ? (rest - usedExtra)
                                          : Math.floor(rest * (weights[e] / weightSum))
            if (extra < 0)
                extra = 0
            widths.push(mins[e] + extra)
            if (e !== lastDrawn)
                usedExtra += extra
        }
        return widths
    }

    /// Partition chip title — Explorer style "本地磁盘 (C:)" / "新加卷 (E:)".
    function partitionBarTitle(volumeData) {
        if (!volumeData)
            return ""
        if (volumeData.unallocated === true)
            return volumeData.name || ""
        if (volumeData.title && String(volumeData.title).length > 0)
            return volumeData.title
        var letter = (volumeData.letter || "").trim()
        if (letter.length === 1)
            letter = letter + ":"
        var name = (volumeData.name || "").trim()
        if (letter.length > 0 && name.length > 0)
            return name + " (" + letter + ")"
        if (letter.length > 0)
            return letter
        return name
    }

    // Shared disk row — icon + name + proportional partition bar.
    // Source rows: drag onto a Target row. Target rows: DropArea accepts source drags.
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
        height: 68
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

        /// Partition bar: source disks show backup layout (incl. "Not backed up");
        /// mapped targets preview post-restore layout (capacity padding + manual edge resize).
        readonly property bool isMappedTargetPreview: !rowRoot.showMapping
                && root.sourceDiskNumberMappedToTarget(rowRoot.diskNumber) >= 0
        property var displayVolumes: {
            var _e = root.mappingEpoch
            var _le = root.layoutEditEpoch
            var _src = root.sourceDisks
            if (!rowRoot.showMapping) {
                var srcNum = root.sourceDiskNumberMappedToTarget(rowRoot.diskNumber)
                if (srcNum >= 0) {
                    var srcDisk = root.sourceDiskDataByNumber(srcNum)
                    if (srcDisk)
                        return root.displayVolumesForMappedTarget(srcDisk, diskData)
                }
            }
            return root.displayVolumesForDisk(diskData)
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

        // Target drop zone: map source_disk → this disk (whole row).
        // Only active while a source disk drag is in progress so edge-resize
        // MouseAreas on the partition bar keep receiving normal mouse input.
        DropArea {
            id: targetDrop
            anchors.fill: parent
            z: 15
            keys: ["aegra.restore.sourceDisk"]
            enabled: !rowRoot.showMapping && rowRoot.diskNumber >= 0
                     && root.mappingDragActive
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

                // Disk identity only — drag the whole source disk (not partition volumes).
                Item {
                    id: diskIdentity
                    Layout.preferredWidth: 138
                    Layout.minimumWidth: 120
                    Layout.fillHeight: true
                    Layout.alignment: Qt.AlignVCenter

                    RowLayout {
                        anchors.fill: parent
                        spacing: 10
                        DiskIcon {
                            Layout.alignment: Qt.AlignVCenter
                            size: 28
                            variant: (rowRoot.diskData && rowRoot.diskData.isSystemDisk)
                                     ? "system" : "hdd"
                        }
                        Column {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 1
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: (rowRoot.diskData && rowRoot.diskData.name)
                                      ? rowRoot.diskData.name : ""
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: {
                                    if (!rowRoot.diskData)
                                        return ""
                                    var style = rowRoot.diskData.partitionStyle
                                                || rowRoot.diskData.type || ""
                                    if (style.indexOf("GPT") >= 0 || style.indexOf("MBR") >= 0)
                                        return "Basic ("
                                               + (style.indexOf("GPT") >= 0 ? "GPT" : "MBR") + ")"
                                    return style.length > 0 ? style : "Basic (GPT)"
                                }
                                color: Theme.colorTextGrey
                                font.pixelSize: 10
                                font.family: Theme.fontFamily
                            }
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: (rowRoot.diskData && rowRoot.diskData.size)
                                      ? rowRoot.diskData.size : ""
                                color: Theme.colorTextGrey
                                font.pixelSize: 10
                                font.family: Theme.fontFamily
                            }
                        }
                    }

                    MouseArea {
                        id: sourceDragMouse
                        anchors.fill: parent
                        z: 20
                        // Disk restore: drag only the disk handle, never partition chips.
                        enabled: rowRoot.showMapping && rowRoot.diskNumber >= 0
                        hoverEnabled: true
                        preventStealing: true
                        cursorShape: {
                            if (!enabled)
                                return Qt.ArrowCursor
                            return pressed || drag.active ? Qt.ClosedHandCursor
                                                          : Qt.OpenHandCursor
                        }
                        drag.target: dragProxy
                        drag.threshold: 6
                        drag.axis: Drag.XAndYAxis
                        onPressed: function(mouse) {
                            root.beginMappingDrag()
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
                            root.endMappingDrag()
                        }
                        onCanceled: root.endMappingDrag()
                    }
                }

                // Partition bar: source = display only; mapped target = resizable edges into Unallocated.
                Rectangle {
                    id: barBg
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    Layout.alignment: Qt.AlignVCenter
                    radius: 3
                    color: Theme.colorInput
                    border.width: 1
                    border.color: Theme.colorBorder
                    clip: !rowRoot.isMappedTargetPreview
                    // Edge-resize drag lives on barBg (not inside Repeater). Updating
                    // layoutEditEpoch rebuilds segment delegates; this MouseArea survives.
                    property int resizeIndex: -1
                    property string resizeEdge: ""
                    property real resizeLastX: 0
                    property int resizeHoverIndex: -1
                    property string resizeHoverEdge: ""

                    // Hit near the data volume's outer edges (into the volume and a little
                    // into the adjacent Unallocated) so grips sit on the volume boundary.
                    function resizeHitAt(x) {
                        if (!rowRoot.isMappedTargetPreview)
                            return null
                        var vols = rowRoot.displayVolumes
                        var widths = partsRow.segmentWidths
                        if (!vols || !widths)
                            return null
                        var zoneIn = 12   // px inside the data volume
                        var zoneOut = 10  // px into adjacent unallocated
                        var cx = 0
                        var spacing = partsRow.spacing
                        var best = null
                        var bestDist = 1e9
                        for (var i = 0; i < vols.length; ++i) {
                            var w = (i < widths.length) ? Number(widths[i]) : 0
                            if (w <= 0)
                                continue
                            var left = cx
                            var right = cx + w
                            var canL = vols[i] && vols[i].canResizeLeft === true
                            var canR = vols[i] && vols[i].canResizeRight === true
                            if (canL) {
                                var l0 = left - zoneOut
                                var l1 = left + zoneIn
                                if (x >= l0 && x <= l1) {
                                    var dl = Math.abs(x - left)
                                    if (dl < bestDist) {
                                        bestDist = dl
                                        best = { index: i, edge: "left" }
                                    }
                                }
                            }
                            if (canR) {
                                var r0 = right - zoneIn
                                var r1 = right + zoneOut
                                if (x >= r0 && x <= r1) {
                                    var dr = Math.abs(x - right)
                                    if (dr < bestDist) {
                                        bestDist = dr
                                        best = { index: i, edge: "right" }
                                    }
                                }
                            }
                            cx = right + spacing
                        }
                        return best
                    }

                    Row {
                        id: partsRow
                        anchors.fill: parent
                        anchors.margins: 1
                        spacing: 0
                        // Recomputed when the bar resizes or volumes change.
                        property var segmentWidths: root.partitionBarWidths(
                                                        rowRoot.displayVolumes,
                                                        partsRow.width,
                                                        partsRow.spacing)
                        property real bytesPerPixel: {
                            var cap = Number(rowRoot.diskData && rowRoot.diskData.capacityBytes) || 0
                            var w = Math.max(1, partsRow.width)
                            return cap > 0 ? (cap / w) : 0
                        }

                        Repeater {
                            model: rowRoot.displayVolumes
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                property bool isUnalloc: modelData && modelData.unallocated === true
                                property bool isNotBackedUp: modelData
                                                             && modelData.notBackedUp === true
                                property bool isHatchStyle: isUnalloc || isNotBackedUp
                                property bool canResizeLeft: rowRoot.isMappedTargetPreview
                                        && modelData && modelData.canResizeLeft === true
                                property bool canResizeRight: rowRoot.isMappedTargetPreview
                                        && modelData && modelData.canResizeRight === true
                                property bool edgeActiveLeft: canResizeLeft
                                        && ((barBg.resizeIndex === index
                                             && barBg.resizeEdge === "left")
                                            || (barBg.resizeHoverIndex === index
                                                && barBg.resizeHoverEdge === "left"))
                                property bool edgeActiveRight: canResizeRight
                                        && ((barBg.resizeIndex === index
                                             && barBg.resizeEdge === "right")
                                            || (barBg.resizeHoverIndex === index
                                                && barBg.resizeHoverEdge === "right"))
                                // usedRatio: 0–1 when freeBytes known; -1 = unknown (uniform fill).
                                property real usedRatio: {
                                    if (isHatchStyle || !modelData)
                                        return -1
                                    var cap = Number(modelData.totalBytes) || 0
                                    var free = Number(modelData.freeBytes)
                                    if (cap <= 0 || isNaN(free) || free < 0)
                                        return -1
                                    var used = cap - free
                                    if (used < 0)
                                        used = 0
                                    if (used > cap)
                                        used = cap
                                    return used / cap
                                }
                                height: partsRow.height
                                width: {
                                    var widths = partsRow.segmentWidths
                                    if (widths && index >= 0 && index < widths.length)
                                        return widths[index]
                                    if (root.isPlaceholderUnallocated(modelData))
                                        return 0
                                    return isHatchStyle ? 12 : 56
                                }
                                visible: width > 0
                                radius: 2
                                // Free band (or full chip when used is unknown / hatch).
                                color: {
                                    if (isHatchStyle)
                                        return Theme.colorUnallocated
                                    var c = Theme.colorAccentBlue
                                    // Known free: light free background; unknown: medium solid.
                                    var a = usedRatio >= 0 ? 0.20 : 0.45
                                    return Qt.rgba(c.r, c.g, c.b, a)
                                }
                                border.width: 0
                                // Keep used-fill clipped; edge grips sit on the outer boundary.
                                clip: false

                                // Used space: stronger fill from the left (inventory freeBytes).
                                Rectangle {
                                    visible: !isHatchStyle && usedRatio > 0
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: Math.round(parent.width * Math.min(1, usedRatio))
                                    clip: true
                                    color: {
                                        var c = Theme.colorAccentBlue
                                        return Qt.rgba(c.r, c.g, c.b, 0.72)
                                    }
                                    z: 0
                                }

                                Canvas {
                                    anchors.fill: parent
                                    visible: isHatchStyle
                                    z: 0
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
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 6
                                    width: parent.width - 14
                                    horizontalAlignment: Text.AlignLeft
                                    z: 1
                                    text: {
                                        if (!modelData)
                                            return ""
                                        if (isUnalloc || isNotBackedUp) {
                                            var uName = modelData.name || ""
                                            if (uName.length === 0) {
                                                //% "Unallocated"
                                                uName = isNotBackedUp
                                                    //% "Not backed up"
                                                    ? qsTrId("aegra.restore.not_backed_up")
                                                    : qsTrId("aegra.restore.unallocated")
                                            }
                                            var uSz = modelData.size || ""
                                            if (parent.width < 40)
                                                return uName
                                            if (uSz.length === 0)
                                                return uName
                                            return uName + "\n" + uSz
                                        }
                                        var title = root.partitionBarTitle(modelData)
                                        var sz = modelData.size || ""
                                        var fs = modelData.fileSystem || ""
                                        var letter = (modelData.letter || "").trim()
                                        if (letter.length === 0) {
                                            if (sz.length === 0)
                                                return title
                                            return title + "\n" + sz
                                        }
                                        if (parent.width < 56)
                                            return title
                                        return title + "\n" + sz + (fs ? (" " + fs) : "")
                                    }
                                    color: isHatchStyle ? Theme.colorUnallocatedText
                                                        : Theme.colorVolumeText
                                    font.pixelSize: parent.width < 90 ? 9 : 10
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    wrapMode: Text.WordWrap
                                    elide: Text.ElideRight
                                    maximumLineCount: 3
                                }

                                // Grips fully inside the volume, flush to its left/right edges.
                                // Do not straddle into the next segment — Row siblings paint
                                // later and would cover any overflow on the right.
                                Item {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 10
                                    z: 6
                                    visible: canResizeLeft
                                    Rectangle {
                                        width: 4
                                        height: parent.height - 6
                                        radius: 2
                                        anchors.left: parent.left
                                        anchors.leftMargin: 1
                                        anchors.verticalCenter: parent.verticalCenter
                                        color: Theme.colorTextWhite
                                        border.width: 1
                                        border.color: Theme.colorAccentBlue
                                        opacity: edgeActiveLeft ? 1.0 : 0.85
                                    }
                                }

                                Item {
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 10
                                    z: 6
                                    visible: canResizeRight
                                    Rectangle {
                                        width: 4
                                        height: parent.height - 6
                                        radius: 2
                                        anchors.right: parent.right
                                        anchors.rightMargin: 1
                                        anchors.verticalCenter: parent.verticalCenter
                                        color: Theme.colorTextWhite
                                        border.width: 1
                                        border.color: Theme.colorAccentBlue
                                        opacity: edgeActiveRight ? 1.0 : 0.85
                                    }
                                }
                            }
                        }
                    }

                    // Survives layoutEditEpoch / Repeater rebuild during drag.
                    MouseArea {
                        id: barResizeMouse
                        anchors.fill: partsRow
                        z: 30
                        enabled: rowRoot.isMappedTargetPreview
                        hoverEnabled: true
                        preventStealing: true
                        acceptedButtons: Qt.LeftButton
                        cursorShape: (barBg.resizeIndex >= 0
                                      || barBg.resizeHoverIndex >= 0)
                                     ? Qt.SizeHorCursor : Qt.ArrowCursor

                        function clearHover() {
                            barBg.resizeHoverIndex = -1
                            barBg.resizeHoverEdge = ""
                        }

                        function endResize() {
                            barBg.resizeIndex = -1
                            barBg.resizeEdge = ""
                            root.layoutResizeActive = false
                        }

                        onPressed: function(mouse) {
                            var hit = barBg.resizeHitAt(mouse.x)
                            if (!hit) {
                                mouse.accepted = false
                                return
                            }
                            var srcNum = root.sourceDiskNumberMappedToTarget(rowRoot.diskNumber)
                            var srcDisk = root.sourceDiskDataByNumber(srcNum)
                            if (srcDisk)
                                root.ensureTargetLayoutEdit(srcDisk, rowRoot.diskData)
                            barBg.resizeIndex = hit.index
                            barBg.resizeEdge = hit.edge
                            barBg.resizeLastX = mouse.x
                            root.layoutResizeActive = true
                        }
                        onPositionChanged: function(mouse) {
                            if (barBg.resizeIndex < 0) {
                                var hit = barBg.resizeHitAt(mouse.x)
                                if (hit) {
                                    barBg.resizeHoverIndex = hit.index
                                    barBg.resizeHoverEdge = hit.edge
                                } else {
                                    clearHover()
                                }
                                return
                            }
                            var dx = mouse.x - barBg.resizeLastX
                            barBg.resizeLastX = mouse.x
                            var bpp = partsRow.bytesPerPixel
                            if (bpp <= 0 || dx === 0)
                                return
                            // left edge: drag left grows; right edge: drag right grows
                            var deltaBytes = Math.round(
                                (barBg.resizeEdge === "left" ? -dx : dx) * bpp)
                            if (deltaBytes === 0)
                                return
                            var newIdx = root.resizeMappedTargetSegment(
                                rowRoot.diskNumber, barBg.resizeIndex,
                                barBg.resizeEdge, deltaBytes)
                            if (newIdx >= 0)
                                barBg.resizeIndex = newIdx
                        }
                        onReleased: endResize()
                        onCanceled: endResize()
                        onExited: {
                            if (barBg.resizeIndex < 0)
                                clearHover()
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
        }
    }


    // Volume row — source (drag) or target (drop).
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
        height: 52
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
            z: 20
            enabled: volRoot.showMapping && volRoot.volumeIndex >= 0
            hoverEnabled: true
            preventStealing: true
            cursorShape: {
                if (!enabled)
                    return Qt.ArrowCursor
                return pressed || drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            }
            drag.target: volDragProxy
            drag.threshold: 6
            drag.axis: Drag.XAndYAxis
            onPressed: function(mouse) {
                root.beginMappingDrag()
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
                root.endMappingDrag()
            }
            onCanceled: root.endMappingDrag()
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
                                         //% "(drag onto a target volume)"
                                         ? qsTrId("aegra.restore.source_volume_hint")
                                         //% "(drag the disk onto a target disk)"
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
                            // Freeze scroll while a mapping drag is in progress.
                            interactive: !root.mappingDragActive
                            boundsBehavior: Flickable.StopAtBounds
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
                            interactive: !root.mappingDragActive
                            boundsBehavior: Flickable.StopAtBounds
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
                            interactive: !root.mappingDragActive && !root.layoutResizeActive
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
                            interactive: !root.mappingDragActive
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
                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        visible: !root.isVolumeMode && !root.isFileMode
                        //% "When the target disk is larger than the source, free space stays unallocated. Drag the edges of a data partition on the Target bar to grow or shrink it into adjacent unallocated space before restore (NTFS/ReFS only)."
                        text: qsTrId("aegra.restore.target_resize_hint")
                        color: Theme.colorTextGrey
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: root.isVolumeMode && !serviceClient.ntfsShrinkAvailable
                        //% "Volume restore writes one backup volume onto an existing non-system volume of equal or larger size. Partition layout is not changed."
                        text: qsTrId("aegra.restore.volume_options_hint")
                        color: Theme.colorTextGrey
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.isVolumeMode && serviceClient.ntfsShrinkAvailable
                        //% "Volume restore writes onto a non-system target. A smaller NTFS target is analyzed before its mapping is accepted; failure after write needs a full retry."
                        text: qsTrId("aegra.restore.volume_options_hint_shrink")
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
                    border.color: Theme.colorAccentBlue
                    visible: root.shrinkConfirmOpen && root.isVolumeMode
                    implicitHeight: shrinkConfirmCol.implicitHeight + 32

                    ColumnLayout {
                        id: shrinkConfirmCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 16
                        spacing: 10

                        Text {
                            Layout.fillWidth: true
                            //% "NTFS shrink plan ready"
                            text: qsTrId("aegra.restore.shrink_confirm_title")
                            color: Theme.colorTextWhite
                            font.pixelSize: 14
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.fillWidth: true
                            //% "Source size: %1"
                            text: qsTrId("aegra.restore.shrink_source_size").arg(
                                      serviceClient.formatBytes(
                                          Number(root.shrinkConfirmDetails.logicalSizeBytes) || 0))
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            //% "Target size: %1"
                            text: qsTrId("aegra.restore.shrink_target_size").arg(
                                      serviceClient.formatBytes(
                                          Number(root.shrinkConfirmDetails.targetCapacityBytes) || 0))
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            //% "Minimum target: %1"
                            text: qsTrId("aegra.restore.shrink_minimum_target").arg(
                                      serviceClient.formatBytes(
                                          Number(root.shrinkConfirmDetails.minimumTargetBytes) || 0))
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            //% "Relocation: %1"
                            text: qsTrId("aegra.restore.shrink_relocation").arg(
                                      serviceClient.formatBytes(
                                          Number(root.shrinkConfirmDetails.relocationBytes) || 0))
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            //% "Scratch upper bound: %1"
                            text: qsTrId("aegra.restore.shrink_scratch").arg(
                                      serviceClient.formatBytes(
                                          Number(root.shrinkConfirmDetails.scratchUpperBoundBytes) || 0))
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: (root.shrinkConfirmDetails.restrictionCodes || []).length > 0
                            //% "Restrictions: %1"
                            text: qsTrId("aegra.restore.shrink_restrictions").arg(
                                      (root.shrinkConfirmDetails.restrictionCodes || []).join(", "))
                            color: Theme.colorAccentRed
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            //% "If restore fails after writing begins, the target may be unusable. You must run a full restore again."
                            text: qsTrId("aegra.restore.shrink_retry_warning")
                            color: Theme.colorAccentRed
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 4
                            spacing: 12
                            Item { Layout.fillWidth: true }
                            AppButton {
                                Layout.preferredWidth: 100
                                Layout.preferredHeight: 36
                                //% "Cancel"
                                text: qsTrId("aegra.common.cancel")
                                onClicked: root.cancelShrinkConfirm()
                            }
                            AppButton {
                                Layout.preferredWidth: 120
                                Layout.preferredHeight: 36
                                //% "Confirm restore"
                                text: qsTrId("aegra.restore.shrink_confirm")
                                primary: true
                                enabled: !serviceClient.restoreCommandBusy
                                onClicked: root.confirmShrinkRestore()
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
                         && !root.shrinkConfirmOpen
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
