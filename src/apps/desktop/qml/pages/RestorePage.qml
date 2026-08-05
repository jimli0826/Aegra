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
    /// Source disks from Service GetRecoveryPointLayout (Manifest volumes). Empty until a
    /// checkpoint is selected and the layout query succeeds.
    readonly property var sourceDisks: serviceClient.recoveryPointSourceDisks
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

    readonly property var backupDates: {
        var _dep = serviceClient.recoveryPointCount
        if (!serviceClient.recoveryPoints)
            return []
        return serviceClient.recoveryPoints.backupDateYmds()
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
            serviceClient.loadRecoveryPointLayout("")
            return
        }
        root.selectedCheckpointId = item.fileUuid || ""
        root.pendingLayoutPassword = ""
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
        serviceClient.loadRecoveryPointLayout(root.selectedCheckpointId, root.pendingLayoutPassword)
    }

    Connections {
        target: serviceClient
        function onRecoveryPointLayoutChanged() {
            if (serviceClient.recoveryPointLayoutLoading)
                return
            if (serviceClient.recoveryPointSourceDisks
                    && serviceClient.recoveryPointSourceDisks.length > 0)
                return
            if (root.selectedCheckpointId.length === 0)
                return
            // Layout failed: likely encrypted archive needing a password.
            if (serviceClient.recoveryPointLayoutErrorText
                    && serviceClient.recoveryPointLayoutErrorText.length > 0) {
                passwordDialog.errorText = serviceClient.recoveryPointLayoutErrorText
                passwordDialog.open()
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
    component DiskRow: Rectangle {
        id: rowRoot
        property var diskData: ({})
        property bool showSystem: false
        width: parent ? parent.width : 100
        height: 68
        radius: 6
        color: Theme.colorListItem
        border.width: (showSystem && diskData && diskData.isSystemDisk) ? 2 : 1
        border.color: (showSystem && diskData && diskData.isSystemDisk)
                      ? "#e74c3c" : Theme.colorBorder

        property var displayVolumes: root.displayVolumesForDisk(diskData)

        RowLayout {
            anchors.fill: parent
            anchors.margins: 8
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
                                //% "Source Disks"
                                text: qsTrId("aegra.restore.source_disks")
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                //% "(from backup image → pick target below)"
                                text: qsTrId("aegra.restore.source_hint")
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
                            model: root.sourceDisks
                            delegate: DiskRow {
                                required property var modelData
                                width: sourceList.width
                                diskData: modelData
                                showSystem: false
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
                                //% "Target Disks"
                                text: qsTrId("aegra.restore.target_disks")
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                //% "(this PC — available restore destinations)"
                                text: qsTrId("aegra.restore.target_hint")
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
                            boundsBehavior: Flickable.StopAtBounds
                            readonly property bool needsScroll: contentHeight > height + 1
                            model: root.targetDisks
                            delegate: DiskRow {
                                required property var modelData
                                width: targetList.width - (targetList.needsScroll ? 12 : 0)
                                diskData: modelData
                                showSystem: false
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
                        //% "When the target disk is larger than the source, grow the last data partition (and filesystem) into free space so no large unallocated region remains. Uncheck to leave free space unallocated. Note: FAT/FAT32 volumes cannot be auto-expanded (Windows does not support online extend); free space stays unallocated."
                        text: qsTrId("aegra.restore.auto_extend_hint")
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
                Layout.preferredWidth: 140
                Layout.preferredHeight: 40
                //% "Restore"
                text: qsTrId("aegra.nav.restore")
                primary: true
                enabled: false
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
