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
    /// "disk" = full-disk restore; "volume" = volume→volume restore.
    property string restoreMode: "disk"
    readonly property bool isVolumeMode: root.restoreMode === "volume"

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

    function setRestoreMode(mode) {
        if (mode !== "disk" && mode !== "volume")
            return
        if (root.restoreMode === mode)
            return
        root.restoreMode = mode
        root.clearDiskMappings()
        root.clearVolumeMappings()
        if (root.hasCheckpoint && !root.sourceLayoutLoading) {
            if (mode === "disk")
                root.initDefaultMappings()
            else
                root.initDefaultVolumeMappings()
        }
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
    readonly property bool canRestore: serviceClient.connected
                                       && serviceClient.restoreStartAvailable
                                       && !serviceClient.restoreCommandBusy
                                       && root.hasCheckpoint
                                       && root.hasMapping
                                       && !root.sourceLayoutLoading
    readonly property string restoreBlockReason: {
        if (!serviceClient.connected)
            //% "Service is not connected"
            return qsTrId("aegra.error.service.disconnected")
        if (!serviceClient.restoreStartAvailable)
            //% "Service does not support restore"
            return qsTrId("aegra.restore.capability_missing")
        if (serviceClient.restoreCommandBusy)
            //% "A restore command is already in progress"
            return qsTrId("aegra.restore.busy")
        if (!root.hasCheckpoint)
            //% "Select a checkpoint first"
            return qsTrId("aegra.restore.select_checkpoint_first")
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

    function startNextQueuedRestore() {
        var q = root.pendingRestoreQueue || []
        if (q.length === 0) {
            root.multiRestoreActive = false
            // All Jobs accepted — show them on Home Tasks.
            root.navigateHomeRequested()
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
        }
    }

    function startMappedRestore() {
        if (!root.canRestore)
            return
        var pairs = root.isVolumeMode ? root.allMappedVolumePairs()
                                      : root.allMappedPairs()
        if (pairs.length === 0) {
            serviceClient.showToast(root.isVolumeMode
                //% "Choose “Restore to” on a source volume"
                ? qsTrId("aegra.restore.volume_map_required")
                //% "Choose “Restore to” on a source disk"
                : qsTrId("aegra.restore.map_required"))
            return
        }
        root.pendingRestoreQueue = pairs
        root.multiRestoreActive = true
        root.startNextQueuedRestore()
    }

    Connections {
        target: serviceClient
        function onRestoreStartSucceeded() {
            if (root.multiRestoreActive) {
                // Start the next mapped disk (or navigate Home when the queue is empty).
                root.startNextQueuedRestore()
                return
            }
            root.navigateHomeRequested()
        }
        function onRestoreStartFailed(message) {
            root.pendingRestoreQueue = []
            root.multiRestoreActive = false
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
            root.panelCheckpoints =
                    serviceClient.recoveryPoints.checkpointsForDate(root.panelSelectedDate)
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
            root.pendingLayoutPassword = ""
            root.clearDiskMappings()
            root.clearVolumeMappings()
            serviceClient.loadRecoveryPointLayout("")
            return
        }
        root.selectedCheckpointId = item.fileUuid || ""
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
        root.selectedCheckpointLabel = bits.join("  ·  ")
        // Try empty password first (unencrypted). Encrypted archives prompt for password.
        serviceClient.loadRecoveryPointLayout(root.selectedCheckpointId, "")
    }

    function submitLayoutPassword(password) {
        if (!root.selectedCheckpointId || root.selectedCheckpointId.length === 0)
            return
        root.pendingLayoutPassword = password || ""
        root.clearDiskMappings()
        root.clearVolumeMappings()
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

    Component.onCompleted: {
        if (serviceClient.connected)
            serviceClient.refreshRepository()
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
            if ((rowRoot.dropHover && rowRoot.dropAccepted) || rowRoot.highlightAsTarget)
                return 2
            return 1
        }
        border.color: {
            // Only highlight droppable targets; invalid ones stay normal (toast on drop).
            if (rowRoot.dropHover && rowRoot.dropAccepted)
                return Theme.colorAccentBlue
            if (rowRoot.highlightAsTarget)
                return root.restoreTargetBorder
            return Theme.colorBorder
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
                                                 : Theme.volumeColor(index)
                                border.width: 1
                                border.color: Theme.colorBorder
                                opacity: isUnalloc ? 1.0 : 0.92
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
                        radius: 4
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
                            color: Theme.colorCard
                            radius: 4
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                    }
                    delegate: ItemDelegate {
                        width: mapCombo.width
                        height: 28
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
                            color: parent.highlighted ? Theme.colorHover
                                                      : "transparent"
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
            if ((volRoot.dropHover && volRoot.dropAccepted) || volRoot.highlightAsTarget)
                return 2
            return 1
        }
        border.color: {
            if (volRoot.dropHover && volRoot.dropAccepted)
                return Theme.colorAccentBlue
            if (volRoot.highlightAsTarget)
                return root.restoreTargetBorder
            return Theme.colorBorder
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
                                      ? (volRoot.volumeData.letter + ": ") : ""
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
                        radius: 4
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
                            color: Theme.colorCard
                            radius: 4
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                    }
                    delegate: ItemDelegate {
                        width: volMapCombo.width
                        height: 28
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
                            color: parent.highlighted ? Theme.colorHover : "transparent"
                        }
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            Row {
                spacing: 8
                Rectangle {
                    width: 3
                    height: 20
                    color: Theme.colorAccentBlue
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    //% "Restore"
                    text: qsTrId("aegra.nav.restore")
                    color: Theme.colorTextWhite
                    font.pixelSize: 18
                    font.bold: true
                    font.family: Theme.fontFamily
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Item { Layout.fillWidth: true }
            // Disk / Volume restore mode
            Row {
                spacing: 0
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    width: Math.max(88, diskModeLabel.implicitWidth + 20)
                    height: 28
                    radius: 4
                    color: !root.isVolumeMode ? Theme.colorAccentBlue : Theme.colorInput
                    border.width: 1
                    border.color: Theme.colorBorder
                    Text {
                        id: diskModeLabel
                        anchors.centerIn: parent
                        //% "Disk"
                        text: qsTrId("aegra.restore.mode_disk")
                        color: Theme.colorTextWhite
                        font.pixelSize: 12
                        font.bold: !root.isVolumeMode
                        font.family: Theme.fontFamily
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.setRestoreMode("disk")
                    }
                }
                Rectangle {
                    width: Math.max(88, volModeLabel.implicitWidth + 20)
                    height: 28
                    radius: 4
                    color: root.isVolumeMode ? Theme.colorAccentBlue : Theme.colorInput
                    border.width: 1
                    border.color: Theme.colorBorder
                    Text {
                        id: volModeLabel
                        anchors.centerIn: parent
                        //% "Volume"
                        text: qsTrId("aegra.restore.mode_volume")
                        color: Theme.colorTextWhite
                        font.pixelSize: 12
                        font.bold: root.isVolumeMode
                        font.family: Theme.fontFamily
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.setRestoreMode("volume")
                    }
                }
            }
        }

        // Main: Source + Target (left) | Options (right) — old 2/3 | 1/3
        RowLayout {
            id: mainSplitRow
            Layout.fillWidth: true
            Layout.fillHeight: true
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
                    radius: 4
                    border.width: 1
                    border.color: Theme.colorBorder

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
                                text: root.isVolumeMode
                                      //% "Source Volumes"
                                      ? qsTrId("aegra.restore.source_volumes")
                                      //% "Source Disks"
                                      : qsTrId("aegra.restore.source_disks")
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                text: root.isVolumeMode
                                      //% "(drag onto a target volume, or use Restore to)"
                                      ? qsTrId("aegra.restore.source_volume_hint")
                                      //% "(drag onto a target disk, or use Restore to)"
                                      : qsTrId("aegra.restore.source_hint")
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            AppButton {
                                //% "Select checkpoint"
                                text: qsTrId("aegra.restore.select_checkpoint")
                                Layout.preferredHeight: 28
                                // Do not let the fill-width hint squeeze this label.
                                Layout.preferredWidth: Math.max(148, implicitWidth)
                                Layout.minimumWidth: Math.max(148, implicitWidth)
                                onClicked: root.openCheckpointPanel()
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
                            id: sourceList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 10
                            visible: !root.isVolumeMode
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
                            visible: root.isVolumeMode
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
                    radius: 4
                    border.width: 1
                    border.color: Theme.colorBorder

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
                                text: root.isVolumeMode
                                      //% "Target Volumes"
                                      ? qsTrId("aegra.restore.target_volumes")
                                      //% "Target Disks"
                                      : qsTrId("aegra.restore.target_disks")
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                text: root.isVolumeMode
                                      //% "(this PC — drop a source volume here)"
                                      ? qsTrId("aegra.restore.target_volume_hint")
                                      //% "(this PC — drop a source disk here)"
                                      : qsTrId("aegra.restore.target_hint")
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }

                        ListView {
                            id: targetList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 10
                            visible: !root.isVolumeMode
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
                            visible: root.isVolumeMode
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
                radius: 4
                border.width: 1
                border.color: Theme.colorBorder
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

                    CheckBox {
                        id: preserveBox
                        Layout.fillWidth: true
                        visible: !root.isVolumeMode
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
                        visible: !root.isVolumeMode
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
                        visible: !root.isVolumeMode
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
                        visible: !root.isVolumeMode
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

                    Item { Layout.fillHeight: true }
                }
            }
        }

        // Footer Restore button (old layout — bottom right, not inside Options)
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            spacing: 12
            Item { Layout.fillWidth: true }
            AppButton {
                id: restoreButton
                Layout.preferredWidth: 140
                Layout.preferredHeight: 40
                text: {
                    if (serviceClient.restoreCommandBusy)
                        //% "Restoring..."
                        return qsTrId("aegra.restore.restoring")
                    //% "Restore"
                    return qsTrId("aegra.nav.restore")
                }
                primary: true
                enabled: root.canRestore
                ToolTip.delay: 400
                ToolTip.visible: restoreButton.hovered && !root.canRestore
                                 && root.restoreBlockReason.length > 0
                ToolTip.text: root.restoreBlockReason
                onClicked: root.startMappedRestore()
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
