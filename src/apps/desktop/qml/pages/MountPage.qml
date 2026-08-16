import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Visual baseline: backup/src/gui MountPage (Source + Mounted | Options).
Item {
    id: root
    //% "Mount"
    Accessible.name: qsTrId("aegra.nav.mount")

    /// Options expanded by default (old MountPage: optionsCollapsed: false).
    property bool optionsCollapsed: false
    readonly property int optionsCollapsedWidth: 36
    property bool checkpointPanelOpen: false
    /// Vertical split between Source / Mounted panes (0.2 … 0.8)
    property real sourceMountedRatio: 0.45
    /// Horizontal split: Options width share when expanded (0.18 … 0.45)
    property real optionsPaneRatio: 0.30
    property string selectedCheckpointId: ""
    property string selectedCheckpointLabel: ""
    property string panelSelectedDate: ""
    property var panelCheckpoints: []
    property int panelCheckpointsEpoch: 0
    property string preferredDriveLetter: ""
    property string pendingLayoutPassword: ""
    /// diskNumber (string key) → checked
    property var selectedDisks: ({})
    property int selectedDisksEpoch: 0
    /// sessionId → checked (multi-select unmount, old MountPage)
    property var selectedSessions: ({})
    property int selectedSessionsEpoch: 0
    property var unmountQueue: []
    /// Free drive letters only (Auto + unused C:–Z:), refreshed from ServiceClient.
    property var driveLetterModel: []
    property int driveLetterModelEpoch: 0

    readonly property var sourceDisks: serviceClient.recoveryPointSourceDisks
    readonly property bool sourceLayoutLoading: serviceClient.recoveryPointLayoutLoading
    readonly property string sourceLayoutError: serviceClient.recoveryPointLayoutErrorText
    readonly property var mountSessions: serviceClient.mountSessions
    readonly property bool mountBusy: serviceClient.mountCommandBusy
    readonly property bool mountSessionsLoading: serviceClient.mountSessionsLoading

    readonly property var backupDates: {
        var _dep = serviceClient.recoveryPointCount
        if (!serviceClient.recoveryPoints)
            return []
        return serviceClient.recoveryPoints.backupDateYmds()
    }

    readonly property int checkedDiskCount: {
        var _e = root.selectedDisksEpoch
        var n = 0
        var map = root.selectedDisks || {}
        for (var k in map) {
            if (map[k])
                ++n
        }
        return n
    }

    readonly property var checkedDiskNumbers: {
        var _e = root.selectedDisksEpoch
        var map = root.selectedDisks || {}
        var disks = root.sourceDisks || []
        var out = []
        for (var i = 0; i < disks.length; ++i) {
            var num = Number(disks[i].diskNumber)
            if (map[String(num)])
                out.push(num)
        }
        return out
    }

    readonly property bool canMount: serviceClient.connected
                                     && serviceClient.mountStartAvailable
                                     && !root.mountBusy
                                     && root.selectedCheckpointId.length > 0
                                     && root.checkedDiskCount > 0
                                     && !root.sourceLayoutLoading

    readonly property int selectedSessionCount: {
        var _e = root.selectedSessionsEpoch
        var n = 0
        var map = root.selectedSessions || {}
        for (var k in map) {
            if (map[k])
                ++n
        }
        return n
    }

    readonly property bool canUnmount: serviceClient.connected
                                       && serviceClient.mountListAvailable
                                       && !root.mountBusy
                                       && root.selectedSessionCount > 0
                                       && (root.unmountQueue || []).length === 0

    function clearSelectedDisks() {
        root.selectedDisks = ({})
        root.selectedDisksEpoch++
    }

    function clearSelectedSessions() {
        root.selectedSessions = ({})
        root.selectedSessionsEpoch++
    }

    function isDiskChecked(diskNumber) {
        var _e = root.selectedDisksEpoch
        return !!(root.selectedDisks && root.selectedDisks[String(diskNumber)])
    }

    function setDiskChecked(diskNumber, checked) {
        var map = Object.assign({}, root.selectedDisks || {})
        if (checked)
            map[String(diskNumber)] = true
        else
            delete map[String(diskNumber)]
        root.selectedDisks = map
        root.selectedDisksEpoch++
    }

    function isSessionChecked(sessionId) {
        var _e = root.selectedSessionsEpoch
        return !!(root.selectedSessions && root.selectedSessions[String(sessionId)])
    }

    function setSessionChecked(sessionId, checked) {
        if (!sessionId || sessionId.length === 0)
            return
        var map = Object.assign({}, root.selectedSessions || {})
        if (checked)
            map[String(sessionId)] = true
        else
            delete map[String(sessionId)]
        root.selectedSessions = map
        root.selectedSessionsEpoch++
    }

    function setAllSessionsSelected(checked) {
        var map = {}
        if (checked) {
            var items = root.mountSessions || []
            for (var i = 0; i < items.length; ++i) {
                var id = items[i].sessionId || ""
                if (id.length > 0)
                    map[id] = true
            }
        }
        root.selectedSessions = map
        root.selectedSessionsEpoch++
    }

    /// Volumes for proportional bar (same rules as RestorePage).
    function displayVolumesForDisk(diskData) {
        var raw = (diskData && diskData.volumes) ? diskData.volumes : []
        var diskTotal = diskData ? (Number(diskData.capacityBytes) || 0) : 0
        var list = []
        var minUnallocBytes = 128 * 1024 * 1024
        var hasPrebuiltUnalloc = false
        var hasOffsets = false
        for (var i = 0; i < raw.length; ++i) {
            if (!raw[i])
                continue
            if (raw[i].unallocated === true)
                hasPrebuiltUnalloc = true
            if (raw[i].offsetBytes !== undefined && raw[i].offsetBytes !== null
                    && raw[i].offsetBytes !== "")
                hasOffsets = true
        }

        function isMsrVolume(v) {
            if (!v)
                return false
            var n = ((v.name || "") + " " + (v.letter || "")).toLowerCase()
            return n.indexOf("microsoft reserved") >= 0 || n.indexOf(" msr") >= 0
                   || n === "msr" || n.indexOf("msr partition") >= 0
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

        function pushUnalloc(bytes) {
            if (bytes <= minUnallocBytes)
                return
            list.push({
                letter: "",
                //% "Unallocated"
                name: qsTrId("aegra.restore.unallocated"),
                size: formatUnallocSize(bytes),
                fileSystem: "",
                totalBytes: bytes,
                unallocated: true
            })
        }

        function pushVolume(v, isUnalloc) {
            if (!isUnalloc && isMsrVolume(v))
                return
            var tb = Number(v.capacityBytes) || 0
            if (isUnalloc && tb <= minUnallocBytes)
                return
            var sz = v.size || ""
            if (isUnalloc && sz.length === 0)
                sz = formatUnallocSize(tb)
            list.push({
                letter: isUnalloc ? "" : (v.letter || ""),
                //% "Unallocated"
                name: isUnalloc ? qsTrId("aegra.restore.unallocated") : (v.name || ""),
                size: sz,
                fileSystem: isUnalloc ? "" : (v.fs || v.fileSystem || ""),
                totalBytes: tb,
                unallocated: isUnalloc
            })
        }

        if (hasPrebuiltUnalloc) {
            for (var p = 0; p < raw.length; ++p) {
                if (!raw[p])
                    continue
                pushVolume(raw[p], raw[p].unallocated === true)
            }
        } else if (hasOffsets) {
            var sorted = []
            for (var s = 0; s < raw.length; ++s) {
                if (raw[s] && !isMsrVolume(raw[s]))
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
                if (off > cursor)
                    pushUnalloc(off - cursor)
                pushVolume(vol, false)
                var end = off + cap
                if (end > cursor)
                    cursor = end
            }
            if (diskTotal > cursor)
                pushUnalloc(diskTotal - cursor)
        } else {
            var sumBytes = 0
            for (var j = 0; j < raw.length; ++j) {
                if (!raw[j] || isMsrVolume(raw[j]))
                    continue
                pushVolume(raw[j], false)
                sumBytes += Number(raw[j].capacityBytes) || 0
            }
            if (diskTotal <= 0 && sumBytes > 0)
                diskTotal = sumBytes
            if (diskTotal > 0 && sumBytes > 0)
                pushUnalloc(diskTotal - sumBytes)
        }

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

    /// Same floor + redistribute rules as RestorePage (EFI stays readable).
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

        var minAlloc = Math.min(56, Math.max(20, Math.floor(avail / Math.max(1, n + 1))))
        var minUnalloc = Math.min(16, minAlloc)

        var mins = []
        var weights = []
        var weightSum = 0
        var minTotal = 0
        for (var i = 0; i < n; ++i) {
            var v = volumes[i]
            var un = v && v.unallocated === true
            var mn = un ? minUnalloc : minAlloc
            mins.push(mn)
            minTotal += mn
            var w = v && v.ratio > 0 ? Number(v.ratio) : 0
            if (w <= 0)
                w = 1.0 / n
            weights.push(w)
            weightSum += w
        }
        if (weightSum <= 0)
            weightSum = n

        var widths = []
        if (minTotal >= avail) {
            var scale = avail / minTotal
            var usedScale = 0
            for (var s = 0; s < n; ++s) {
                var sw = (s === n - 1) ? (avail - usedScale)
                                       : Math.floor(mins[s] * scale)
                widths.push(Math.max(1, sw))
                usedScale += widths[s]
            }
            return widths
        }

        var rest = avail - minTotal
        var usedExtra = 0
        for (var e = 0; e < n; ++e) {
            var extra = (e === n - 1) ? (rest - usedExtra)
                                      : Math.floor(rest * (weights[e] / weightSum))
            if (extra < 0)
                extra = 0
            widths.push(mins[e] + extra)
            usedExtra += (e === n - 1) ? 0 : extra
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

    function imageLabelForSession(item) {
        if (!item)
            return ""
        var id = item.recoveryPointId || ""
        if (id.length === 0)
            return ""
        if (id === root.selectedCheckpointId && root.selectedCheckpointLabel.length > 0)
            return root.selectedCheckpointLabel
        // Shorten UUID for list density when catalog label is unavailable.
        if (id.length > 18)
            return id.substring(0, 8) + "…" + id.substring(id.length - 6)
        return id
    }

    function openCheckpointPanel() {
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

    function applySelectedCheckpoint(item) {
        if (!item) {
            root.selectedCheckpointId = ""
            root.selectedCheckpointLabel = ""
            root.pendingLayoutPassword = ""
            root.clearSelectedDisks()
            serviceClient.loadRecoveryPointLayout("")
            return
        }
        root.selectedCheckpointId = item.fileUuid || ""
        root.pendingLayoutPassword = ""
        root.clearSelectedDisks()
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
        serviceClient.loadRecoveryPointLayout(root.selectedCheckpointId, "")
    }

    function submitLayoutPassword(password) {
        if (!root.selectedCheckpointId || root.selectedCheckpointId.length === 0)
            return
        root.pendingLayoutPassword = password || ""
        root.clearSelectedDisks()
        serviceClient.loadRecoveryPointLayout(root.selectedCheckpointId, root.pendingLayoutPassword)
    }

    function refreshDriveLetterModel() {
        // Free letters only (old MountBackend.driveLetterOptions + MountPage rebuild).
        var opts = serviceClient.availableDriveLetters
                   ? serviceClient.availableDriveLetters()
                   : []
        var out = []
        for (var i = 0; i < opts.length; ++i) {
            var o = opts[i] || {}
            var value = o.value || ""
            out.push({
                label: (value === "")
                       ? qsTrId("aegra.mount.drive_letter_auto")
                       : (o.label || value),
                value: value
            })
        }
        if (out.length === 0)
            out = [{ label: qsTrId("aegra.mount.drive_letter_auto"), value: "" }]
        root.driveLetterModel = out
        root.driveLetterModelEpoch++
        root.syncDriveLetterIndex()
    }

    function syncDriveLetterIndex() {
        if (!driveLetterCombo)
            return
        var want = root.preferredDriveLetter || ""
        // Accept "Z" or "Z:" from older selection state.
        if (want.length === 1)
            want = want + ":"
        var model = root.driveLetterModel || []
        for (var i = 0; i < model.length; ++i) {
            var v = model[i].value || ""
            if (v === want || v === root.preferredDriveLetter) {
                driveLetterCombo.currentIndex = i
                root.preferredDriveLetter = v
                return
            }
        }
        // Preferred letter no longer free — fall back to Auto.
        root.preferredDriveLetter = ""
        driveLetterCombo.currentIndex = 0
    }

    function runMount() {
        if (!root.canMount)
            return
        var disks = root.checkedDiskNumbers || []
        if (disks.length === 0)
            return
        // Multi-disk batch (old MountBackend): preferred letter only for the first disk.
        // Pass a plain array so Q_INVOKABLE receives every checked disk number.
        var diskList = []
        for (var i = 0; i < disks.length; ++i)
            diskList.push(Number(disks[i]))
        serviceClient.startMountDisks(diskList, root.selectedCheckpointId,
                                      root.preferredDriveLetter || "",
                                      root.pendingLayoutPassword || "")
    }

    function runUnmount() {
        if (!root.canUnmount)
            return
        var ids = []
        var map = root.selectedSessions || {}
        for (var k in map) {
            if (map[k])
                ids.push(k)
        }
        if (ids.length === 0)
            return
        root.unmountQueue = ids
        root.pumpUnmountQueue()
    }

    function pumpUnmountQueue() {
        if (root.mountBusy)
            return
        var q = root.unmountQueue || []
        if (q.length === 0)
            return
        var next = q[0]
        var rest = q.slice(1)
        root.unmountQueue = rest
        if (!serviceClient.unmountSession(next)) {
            root.unmountQueue = []
            return
        }
    }

    Connections {
        target: serviceClient
        function onRecoveryPointLayoutChanged() {
            if (serviceClient.recoveryPointLayoutLoading)
                return
            if (serviceClient.recoveryPointSourceDisks
                    && serviceClient.recoveryPointSourceDisks.length > 0) {
                // Default-check the first source disk for a one-click mount path.
                var first = serviceClient.recoveryPointSourceDisks[0]
                if (first && first.diskNumber !== undefined)
                    root.setDiskChecked(Number(first.diskNumber), true)
                return
            }
            if (root.selectedCheckpointId.length === 0)
                return
            if (serviceClient.recoveryPointLayoutErrorText
                    && serviceClient.recoveryPointLayoutErrorText.length > 0) {
                passwordDialog.errorText = serviceClient.recoveryPointLayoutErrorText
                passwordDialog.open()
            }
        }
        function onMountStartSucceeded() {
            root.refreshDriveLetterModel()
        }
        function onMountStartFailed() {
            root.refreshDriveLetterModel()
        }
        function onUnmountSucceeded() {
            // Clear selection for completed id if still present.
            root.clearSelectedSessions()
            if ((root.unmountQueue || []).length > 0)
                root.pumpUnmountQueue()
            else {
                serviceClient.refreshMountSessions()
                root.refreshDriveLetterModel()
            }
        }
        function onUnmountFailed() {
            root.unmountQueue = []
            root.refreshDriveLetterModel()
        }
        function onMountSessionsChanged() {
            // Drop selection for sessions that disappeared.
            var map = Object.assign({}, root.selectedSessions || {})
            var items = serviceClient.mountSessions || []
            var alive = {}
            for (var i = 0; i < items.length; ++i)
                alive[items[i].sessionId || ""] = true
            var changed = false
            for (var k in map) {
                if (!alive[k]) {
                    delete map[k]
                    changed = true
                }
            }
            if (changed) {
                root.selectedSessions = map
                root.selectedSessionsEpoch++
            }
            // Restore source disks if a transient session drop cleared them.
            root.ensureSourceLayout()
            root.refreshDriveLetterModel()
        }
        function onStateChanged() {
            if (!serviceClient.connected)
                return
            serviceClient.refreshMountSessions()
            root.ensureSourceLayout()
            root.refreshDriveLetterModel()
        }
    }

    // Source disk row: checkbox + DiskIcon + name/style/size + volume bar (old MountPage).
    component SourceDiskRow: Rectangle {
        id: rowRoot
        property var diskData: ({})
        property bool checked: false
        property int diskNumber: diskData && diskData.diskNumber !== undefined
                                 ? Number(diskData.diskNumber) : -1
        property var displayVolumes: root.displayVolumesForDisk(diskData)

        width: parent ? parent.width : 100
        height: 68
        radius: 6
        color: Theme.colorListItem
        border.width: 1
        border.color: Theme.colorBorder

        RowLayout {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 10

            CheckBox {
                id: diskCheck
                Layout.preferredWidth: 28
                Layout.alignment: Qt.AlignVCenter
                checked: rowRoot.checked
                onClicked: {
                    if (rowRoot.diskNumber >= 0)
                        root.setDiskChecked(rowRoot.diskNumber, checked)
                }
                indicator: Rectangle {
                    implicitWidth: 18
                    implicitHeight: 18
                    x: diskCheck.leftPadding
                    y: parent.height / 2 - height / 2
                    radius: 3
                    color: diskCheck.checked ? Theme.colorAccentBlue : Theme.colorInput
                    border.color: diskCheck.checked ? Theme.colorAccentBlue : Theme.colorBorder
                    border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: diskCheck.checked ? "\u2713" : ""
                        color: "white"
                        font.pixelSize: 12
                        font.bold: true
                    }
                }
            }

            DiskIcon {
                Layout.alignment: Qt.AlignVCenter
                size: 28
                variant: (rowRoot.diskData && rowRoot.diskData.isSystemDisk) ? "system" : "hdd"
            }

            Column {
                Layout.preferredWidth: 90
                Layout.alignment: Qt.AlignVCenter
                spacing: 1
                Text {
                    text: (rowRoot.diskData && rowRoot.diskData.name)
                          ? rowRoot.diskData.name
                          : (rowRoot.diskNumber >= 0 ? ("Disk " + rowRoot.diskNumber) : "")
                    color: Theme.colorTextWhite
                    font.pixelSize: 12
                    font.bold: true
                    font.family: Theme.fontFamily
                }
                Text {
                    text: {
                        if (!rowRoot.diskData)
                            return ""
                        var style = rowRoot.diskData.partitionStyle || rowRoot.diskData.type || ""
                        if (style.indexOf("GPT") >= 0)
                            return "Basic (GPT)"
                        if (style.indexOf("MBR") >= 0)
                            return "Basic (MBR)"
                        return style.length > 0 ? style : ""
                    }
                    color: Theme.colorTextGrey
                    font.pixelSize: 10
                    font.family: Theme.fontFamily
                }
                Text {
                    text: (rowRoot.diskData && rowRoot.diskData.size) ? rowRoot.diskData.size : ""
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
                    property var segmentWidths: root.partitionBarWidths(
                                                    rowRoot.displayVolumes,
                                                    partsRow.width,
                                                    partsRow.spacing)

                    Repeater {
                        model: rowRoot.displayVolumes
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            property bool isUnalloc: modelData && modelData.unallocated === true
                            height: partsRow.height
                            width: {
                                var widths = partsRow.segmentWidths
                                if (widths && index >= 0 && index < widths.length)
                                    return widths[index]
                                return isUnalloc ? 12 : 56
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
                                        //% "Unallocated"
                                        var uName = modelData.name
                                                    || qsTrId("aegra.restore.unallocated")
                                        var uSz = modelData.size || ""
                                        if (parent.width < 48)
                                            return "\u2026"
                                        if (parent.width < 72 || uSz.length === 0)
                                            return uName
                                        return uName + "\n" + uSz
                                    }
                                    var title = root.partitionBarTitle(modelData)
                                    var sz = modelData.size || ""
                                    var fs = modelData.fileSystem || ""
                                    if (parent.width < 56)
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

        MouseArea {
            anchors.fill: parent
            anchors.leftMargin: 40
            z: -1
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                if (rowRoot.diskNumber < 0)
                    return
                root.setDiskChecked(rowRoot.diskNumber, !rowRoot.checked)
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
                    //% "Mount"
                    text: qsTrId("aegra.nav.mount")
                    color: Theme.colorTextWhite
                    font.pixelSize: 18
                    font.bold: true
                    font.family: Theme.fontFamily
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Item { Layout.fillWidth: true }
        }

        // Main: Source + Mounted (left) | Options (right)
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
                    Layout.preferredHeight: Math.round(1000 * root.sourceMountedRatio)
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
                                //% "Source Disks"
                                text: qsTrId("aegra.mount.source_disks")
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Text {
                                //% "(check disks to mount — volumes auto-get drive letters)"
                                text: qsTrId("aegra.mount.source_hint")
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                                Layout.alignment: Qt.AlignVCenter
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            AppButton {
                                //% "Select checkpoint"
                                text: qsTrId("aegra.restore.select_checkpoint")
                                Layout.preferredHeight: 28
                                Layout.preferredWidth: Math.max(148, implicitWidth)
                                Layout.minimumWidth: Math.max(148, implicitWidth)
                                Layout.alignment: Qt.AlignVCenter
                                onClicked: root.openCheckpointPanel()
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: root.selectedCheckpointId.length > 0
                            //% "Selected:"
                            text: qsTrId("aegra.restore.selected_label")
                                  + " " + (root.selectedCheckpointLabel.length > 0
                                           ? root.selectedCheckpointLabel
                                           : root.selectedCheckpointId)
                            color: Theme.colorAccentBlue
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                            elide: Text.ElideMiddle
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 32
                                wrapMode: Text.WordWrap
                                horizontalAlignment: Text.AlignHCenter
                                visible: root.selectedCheckpointId.length === 0
                                         && !root.sourceLayoutLoading
                                //% "Select a checkpoint to view source disks"
                                text: qsTrId("aegra.mount.source_empty")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }

                            Text {
                                anchors.centerIn: parent
                                visible: root.sourceLayoutLoading
                                //% "Loading"
                                text: qsTrId("aegra.common.loading")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }

                            ListView {
                                anchors.fill: parent
                                clip: true
                                visible: !root.sourceLayoutLoading
                                         && root.selectedCheckpointId.length > 0
                                         && (root.sourceDisks || []).length > 0
                                model: root.sourceDisks || []
                                spacing: 6
                                boundsBehavior: Flickable.StopAtBounds
                                delegate: SourceDiskRow {
                                    width: ListView.view.width
                                    diskData: modelData || ({})
                                    checked: root.isDiskChecked(
                                                 modelData && modelData.diskNumber !== undefined
                                                 ? Number(modelData.diskNumber) : -1)
                                }
                            }
                        }
                    }
                }

                // Thin drag handle: resize Source / Mounted vertically
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
                        opacity: diskSplitMouse.pressed || diskSplitMouse.containsMouse
                                 ? 1.0 : 0.7
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
                            pressRatio = root.sourceMountedRatio
                        }
                        onPositionChanged: function(mouse) {
                            if (!pressed)
                                return
                            var y = mapToItem(disksColumn, 0, mouse.y).y
                            var avail = disksColumn.height - diskSplitter.height
                            if (avail < 80)
                                return
                            var r = pressRatio + (y - pressY) / avail
                            root.sourceMountedRatio = Math.min(0.8, Math.max(0.2, r))
                        }
                    }
                }

                // -------- Mounted --------
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: Math.round(1000 * (1.0 - root.sourceMountedRatio))
                    Layout.minimumHeight: 120
                    color: Theme.colorCard
                    radius: 4
                    border.width: 1
                    border.color: Theme.colorBorder

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8

                        Row {
                            spacing: 8
                            Rectangle {
                                width: 3
                                height: 16
                                color: Theme.colorAccentBlue
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                //% "Mounted"
                                text: qsTrId("aegra.mount.mounted")
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: (root.mountSessions || []).length > 0
                                      ? ("(" + (root.mountSessions || []).length + ")")
                                      : ""
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10
                            spacing: 8

                            CheckBox {
                                id: selectAllMountedCheck
                                Layout.preferredWidth: 28
                                Layout.alignment: Qt.AlignVCenter
                                enabled: (root.mountSessions || []).length > 0
                                checked: {
                                    var _e = root.selectedSessionsEpoch
                                    var total = (root.mountSessions || []).length
                                    return total > 0 && root.selectedSessionCount === total
                                }
                                onClicked: {
                                    var total = (root.mountSessions || []).length
                                    var allOn = total > 0 && root.selectedSessionCount === total
                                    root.setAllSessionsSelected(!allOn)
                                }
                                indicator: Rectangle {
                                    implicitWidth: 18
                                    implicitHeight: 18
                                    x: selectAllMountedCheck.leftPadding
                                    y: parent.height / 2 - height / 2
                                    radius: 3
                                    color: {
                                        if (!selectAllMountedCheck.enabled)
                                            return Theme.colorButtonDisabled
                                        var total = (root.mountSessions || []).length
                                        if (total > 0 && root.selectedSessionCount > 0
                                                && root.selectedSessionCount < total)
                                            return Theme.colorAccentBlue
                                        return selectAllMountedCheck.checked
                                               ? Theme.colorAccentBlue : Theme.colorInput
                                    }
                                    border.color: {
                                        if (!selectAllMountedCheck.enabled)
                                            return Theme.colorBorder
                                        if (selectAllMountedCheck.checked
                                                || root.selectedSessionCount > 0)
                                            return Theme.colorAccentBlue
                                        return Theme.colorBorder
                                    }
                                    border.width: 1
                                    opacity: selectAllMountedCheck.enabled ? 1.0 : 0.5
                                    Text {
                                        anchors.centerIn: parent
                                        text: {
                                            var total = (root.mountSessions || []).length
                                            var n = root.selectedSessionCount
                                            if (n <= 0)
                                                return ""
                                            if (n < total)
                                                return "\u2212"
                                            return "\u2713"
                                        }
                                        color: "white"
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                }
                            }
                            Text {
                                Layout.preferredWidth: 64
                                //% "Drive(s)"
                                text: qsTrId("aegra.mount.col.drives")
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                            }
                            Text {
                                Layout.preferredWidth: 56
                                //% "Disk"
                                text: qsTrId("aegra.mount.col.disk")
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                            }
                            Text {
                                Layout.fillWidth: true
                                //% "Image"
                                text: qsTrId("aegra.mount.col.image")
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                            }
                            Text {
                                Layout.preferredWidth: 72
                                horizontalAlignment: Text.AlignRight
                                //% "Size"
                                text: qsTrId("aegra.mount.col.size")
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: Theme.colorBorder
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            Column {
                                anchors.centerIn: parent
                                width: parent.width - 40
                                spacing: 6
                                visible: !root.mountSessionsLoading
                                         && (root.mountSessions || []).length === 0
                                Text {
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    //% "No mounted images"
                                    text: qsTrId("aegra.mount.mounted_empty")
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                }
                                Text {
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    //% "Check source disk(s), then click Mount"
                                    text: qsTrId("aegra.mount.mounted_hint")
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    wrapMode: Text.WordWrap
                                    lineHeight: 1.4
                                }
                            }

                            ListView {
                                id: mountedList
                                anchors.fill: parent
                                clip: true
                                visible: (root.mountSessions || []).length > 0
                                model: root.mountSessions || []
                                spacing: 4
                                boundsBehavior: Flickable.StopAtBounds
                                readonly property bool needsScroll: contentHeight > height + 1
                                ScrollBar.vertical: ScrollBar {
                                    policy: mountedList.needsScroll ? ScrollBar.AlwaysOn
                                                                    : ScrollBar.AlwaysOff
                                    width: 8
                                    padding: 0
                                    contentItem: Rectangle {
                                        implicitWidth: 6
                                        radius: 3
                                        color: Theme.colorBorder
                                        opacity: parent.pressed ? 1.0
                                                 : (parent.hovered ? 0.9 : 0.65)
                                    }
                                    background: Item {}
                                }

                                delegate: Rectangle {
                                    id: mountedRow
                                    width: mountedList.width
                                           - (mountedList.needsScroll ? 12 : 0)
                                    height: 48
                                    radius: 4
                                    readonly property string sessionId: modelData.sessionId || ""
                                    readonly property bool checked:
                                        root.isSessionChecked(sessionId)
                                    color: checked ? Theme.colorHover
                                                   : (sessionMouse.containsMouse
                                                      ? Theme.colorHover : Theme.colorListItem)
                                    border.width: checked ? 1 : 0
                                    border.color: Theme.colorAccentBlue

                                    MouseArea {
                                        id: sessionMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        z: -1
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.setSessionChecked(
                                                       mountedRow.sessionId, !mountedRow.checked)
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        spacing: 8

                                        CheckBox {
                                            id: mountedCheck
                                            Layout.preferredWidth: 28
                                            Layout.alignment: Qt.AlignVCenter
                                            checked: mountedRow.checked
                                            onClicked: root.setSessionChecked(
                                                           mountedRow.sessionId, checked)
                                            indicator: Rectangle {
                                                implicitWidth: 18
                                                implicitHeight: 18
                                                x: mountedCheck.leftPadding
                                                y: parent.height / 2 - height / 2
                                                radius: 3
                                                color: mountedCheck.checked
                                                       ? Theme.colorAccentBlue : Theme.colorInput
                                                border.color: mountedCheck.checked
                                                              ? Theme.colorAccentBlue
                                                              : Theme.colorBorder
                                                border.width: 1
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: mountedCheck.checked ? "\u2713" : ""
                                                    color: "white"
                                                    font.pixelSize: 12
                                                    font.bold: true
                                                }
                                            }
                                        }
                                        Text {
                                            Layout.preferredWidth: 64
                                            text: modelData.mountPoint || "\u2014"
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 12
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideRight
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                        Text {
                                            Layout.preferredWidth: 56
                                            text: modelData.diskName
                                                  || (modelData.sourceDiskNumber !== undefined
                                                      ? ("Disk " + modelData.sourceDiskNumber)
                                                      : "")
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 11
                                            font.family: Theme.fontFamily
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                        Column {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Text {
                                                width: parent.width
                                                text: root.imageLabelForSession(modelData)
                                                color: Theme.colorTextWhite
                                                font.pixelSize: 12
                                                font.family: Theme.fontFamily
                                                elide: Text.ElideMiddle
                                            }
                                            Text {
                                                width: parent.width
                                                text: modelData.recoveryPointId || ""
                                                color: Theme.colorTextGrey
                                                font.pixelSize: 10
                                                font.family: Theme.fontFamily
                                                elide: Text.ElideMiddle
                                            }
                                        }
                                        Text {
                                            Layout.preferredWidth: 72
                                            horizontalAlignment: Text.AlignRight
                                            text: modelData.sizeText || ""
                                            color: Theme.colorTextGrey
                                            font.pixelSize: 11
                                            font.family: Theme.fontFamily
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Horizontal drag handle
            Item {
                id: optionsSplitter
                Layout.fillHeight: true
                Layout.preferredWidth: root.optionsCollapsed ? 0 : 5
                Layout.minimumWidth: root.optionsCollapsed ? 0 : 5
                Layout.maximumWidth: root.optionsCollapsed ? 0 : 5
                visible: !root.optionsCollapsed
                z: 2
                Rectangle {
                    anchors.centerIn: parent
                    width: 2
                    height: Math.min(36, parent.height - 24)
                    radius: 1
                    color: optSplitMouse.pressed || optSplitMouse.containsMouse
                           ? Theme.colorAccentBlue : Theme.colorBorder
                    opacity: optSplitMouse.pressed || optSplitMouse.containsMouse ? 1.0 : 0.7
                }
                MouseArea {
                    id: optSplitMouse
                    anchors.fill: parent
                    anchors.leftMargin: -3
                    anchors.rightMargin: -3
                    hoverEnabled: true
                    cursorShape: Qt.SplitHCursor
                    preventStealing: true
                    enabled: !root.optionsCollapsed
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
                        var avail = mainSplitRow.width - optionsSplitter.width
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
                            height: mountOptsCollapsedLabel.implicitWidth
                            anchors.horizontalCenter: parent.horizontalCenter
                            Text {
                                id: mountOptsCollapsedLabel
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

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 12
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
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                            radius: 4
                            color: mountOptsCollapseMouse.containsMouse
                                   ? Theme.colorHover : "transparent"
                            border.width: mountOptsCollapseMouse.containsMouse ? 1 : 0
                            border.color: Theme.colorBorder
                            Layout.alignment: Qt.AlignVCenter
                            Text {
                                anchors.centerIn: parent
                                text: "\u25B6"
                                color: Theme.colorTextWhite
                                font.pixelSize: 11
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            MouseArea {
                                id: mountOptsCollapseMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.optionsCollapsed = true
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Text {
                            //% "Drive letter"
                            text: qsTrId("aegra.mount.drive_letter")
                            color: Theme.colorTextWhite
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }

                        ComboBox {
                            id: driveLetterCombo
                            Layout.fillWidth: true
                            Layout.preferredHeight: 32
                            model: root.driveLetterModel
                            textRole: "label"
                            // Rebind when free-letter list refreshes after mount/unmount.
                            property int modelEpoch: root.driveLetterModelEpoch
                            onModelEpochChanged: root.syncDriveLetterIndex()
                            Component.onCompleted: root.syncDriveLetterIndex()
                            onActivated: function(index) {
                                var model = root.driveLetterModel || []
                                if (index < 0 || index >= model.length)
                                    return
                                root.preferredDriveLetter = model[index].value || ""
                            }

                            background: Rectangle {
                                color: Theme.colorInput
                                radius: 8
                                border.width: 1
                                border.color: Theme.colorBorder
                            }
                            indicator: ComboBoxIndicator { combo: driveLetterCombo }
                            contentItem: Text {
                                leftPadding: 10
                                rightPadding: driveLetterCombo.indicator
                                              ? driveLetterCombo.indicator.width + 10 : 8
                                text: driveLetterCombo.displayText
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                            popup: Popup {
                                y: driveLetterCombo.height
                                width: driveLetterCombo.width
                                implicitHeight: Math.min(contentItem.implicitHeight + 4, 240)
                                padding: 2
                                contentItem: ListView {
                                    clip: true
                                    implicitHeight: contentHeight
                                    model: driveLetterCombo.popup.visible
                                           ? driveLetterCombo.delegateModel : null
                                    currentIndex: driveLetterCombo.highlightedIndex
                                    ScrollIndicator.vertical: ScrollIndicator { }
                                }
                                background: Rectangle {
                                    color: Theme.colorPopup
                                    border.color: Theme.colorBorder
                                    radius: 8
                                }
                            }
                            delegate: ItemDelegate {
                                id: driveItemDel
                                width: driveLetterCombo.width
                                height: 30
                                hoverEnabled: true
                                highlighted: driveLetterCombo.highlightedIndex === index
                                contentItem: Text {
                                    text: modelData.label || ""
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    radius: 4
                                    color: (driveItemDel.hovered || driveItemDel.highlighted)
                                           ? Theme.colorHover : "transparent"
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            //% "Preferred letter for the first volume. Choose Auto to pick the next free letter. Additional volumes are assigned automatically."
                            text: qsTrId("aegra.mount.drive_letter_hint")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            spacing: 12
            Item { Layout.fillWidth: true }
            AppButton {
                //% "Unmount"
                text: qsTrId("aegra.mount.unmount")
                Layout.preferredWidth: 120
                Layout.preferredHeight: 40
                enabled: root.canUnmount
                onClicked: root.runUnmount()
            }
            AppButton {
                //% "Mount"
                text: qsTrId("aegra.nav.mount")
                Layout.preferredWidth: 140
                Layout.preferredHeight: 40
                enabled: root.canMount
                onClicked: root.runMount()
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

    BackupPasswordDialog {
        id: passwordDialog
        parent: Overlay.overlay
        onAccepted: function(password) { root.submitLayoutPassword(password) }
        onCancelled: { root.pendingLayoutPassword = "" }
    }

    function ensureSourceLayout() {
        if (!serviceClient.connected || root.selectedCheckpointId.length === 0)
            return
        if (root.sourceLayoutLoading)
            return
        if ((root.sourceDisks || []).length > 0)
            return
        serviceClient.loadRecoveryPointLayout(root.selectedCheckpointId,
                                              root.pendingLayoutPassword || "")
    }

    Component.onCompleted: {
        root.refreshDriveLetterModel()
        if (serviceClient.connected) {
            serviceClient.refreshRepository()
            serviceClient.refreshMountSessions()
            root.ensureSourceLayout()
        }
    }

    Connections {
        target: serviceClient
        function onRepositoryChanged() {
            if (root.checkpointPanelOpen)
                root.reloadPanelCheckpoints()
        }
    }
}
