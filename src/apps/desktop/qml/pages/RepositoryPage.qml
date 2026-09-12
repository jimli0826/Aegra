import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Item {
    id: root
    property var pointDetails: ({})
    readonly property int jobsRevision: serviceClient.jobs ? serviceClient.jobs.revision : 0
    /// points: objects with fileUuid, or plain fileUuid strings.
    function pointFileUuid(point) {
        return typeof point === "string" ? point : (point ? point.fileUuid : "")
    }
    /// Merged Verify (operation 3) / Boot Check (operation 5) status for one point:
    /// a live job wins while active or when newer than the persisted outcome;
    /// otherwise the Service-recorded outcome; "na" when the check never ran.
    function recoveryPointCheckStatus(fileUuid, operation) {
        var _r = root.jobsRevision
        var _c = serviceClient.recoveryPointCount
        if (!fileUuid)
            return { key: "na", messageText: "" }
        var live = serviceClient.jobs
                   ? serviceClient.jobs.recoveryPointOperationStatus(fileUuid, operation) : null
        var checks = serviceClient.recoveryPoints
                     ? serviceClient.recoveryPoints.recoveryPointChecks(fileUuid) : null
        var persisted = checks ? (operation === 5 ? checks.bootCheck : checks.verifyCheck) : null
        if (persisted && !persisted.key)
            persisted = null
        var liveKey = live && live.key ? live.key : "none"
        if (liveKey === "running" || liveKey === "queued")
            return { key: liveKey, messageText: "" }
        if (liveKey !== "none"
                && (!persisted
                    || Number(live.createdUtcMs || 0) > Number(persisted.completedUtcMs || 0)))
            return { key: liveKey, messageText: live.messageText || "" }
        if (persisted)
            return { key: persisted.key, messageText: persisted.messageText || "" }
        return { key: "na", messageText: "" }
    }
    function recoveryPointCheckKey(fileUuid, operation) {
        return root.recoveryPointCheckStatus(fileUuid, operation).key
    }
    /// Backup-set aggregate: activity first, then any failure, then "partial" when
    /// only some points were ever checked, "na" when none were.
    function backupSetCheckKey(points, operation) {
        var _r = root.jobsRevision
        var counts = { running: 0, queued: 0, failed: 0, cancelled: 0, succeeded: 0, na: 0 }
        for (var i = 0; i < points.length; ++i) {
            var key = root.recoveryPointCheckKey(root.pointFileUuid(points[i]), operation)
            counts[key in counts ? key : "na"] += 1
        }
        if (counts.running > 0)
            return "running"
        if (counts.queued > 0)
            return "queued"
        if (counts.failed > 0)
            return "failed"
        if (counts.cancelled > 0)
            return "cancelled"
        if (counts.succeeded > 0)
            return counts.na > 0 ? "partial" : "succeeded"
        return "na"
    }
    /// operation 5 = boot check wording; anything else uses the verify wording.
    function checkStatusText(key, operation) {
        var bootCheck = Number(operation) === 5
        if (key === "running")
            //% "Boot checking"
            return bootCheck ? qsTrId("aegra.repository.bootcheck.status.running")
                             : qsTrId("aegra.repository.verify.status.running")
        if (key === "queued")
            //% "Boot check queued"
            return bootCheck ? qsTrId("aegra.repository.bootcheck.status.queued")
                             : qsTrId("aegra.repository.verify.status.queued")
        if (key === "succeeded")
            //% "Boot verified"
            return bootCheck ? qsTrId("aegra.repository.bootcheck.status.succeeded")
                             : qsTrId("aegra.repository.verify.status.succeeded")
        if (key === "failed")
            //% "Boot check failed"
            return bootCheck ? qsTrId("aegra.repository.bootcheck.status.failed")
                             : qsTrId("aegra.repository.verify.status.failed")
        if (key === "cancelled")
            return qsTrId("aegra.repository.verify.status.cancelled")
        if (key === "partial")
            //% "Partially checked"
            return qsTrId("aegra.repository.check.partial")
        //% "N/A"
        return qsTrId("aegra.repository.check.na")
    }
    function recoveryPointCheckLabel(fileUuid, operation) {
        var st = root.recoveryPointCheckStatus(fileUuid, operation)
        if ((st.key === "failed" || st.key === "cancelled") && st.messageText)
            return st.messageText
        return root.checkStatusText(st.key, operation)
    }
    function backupSetCheckLabel(points, operation) {
        var key = root.backupSetCheckKey(points, operation)
        if (key === "failed" || key === "cancelled") {
            for (var i = 0; i < points.length; ++i) {
                var st = root.recoveryPointCheckStatus(root.pointFileUuid(points[i]), operation)
                if (st.key === key && st.messageText)
                    return st.messageText
            }
        }
        return root.checkStatusText(key, operation)
    }
    function checkGlyphKind(key) {
        if (key === "partial")
            return "incomplete"
        if (key === "running" || key === "queued" || key === "succeeded"
                || key === "failed" || key === "cancelled")
            return key
        return "na"
    }
    function backupSetPresentation(group) {
        var schedules = serviceClient.schedules
        for (var i = 0; i < schedules.length; ++i) {
            var schedule = schedules[i]
            if (schedule.backupSetUuid === group.backupSetUuid)
                return { title: schedule.displayName || schedule.sourceName || group.sourceSummary,
                         source: schedule.protectedSourceSummary || group.sourceSummary }
        }
        return { title: group.sourceSummary, source: "" }
    }
    property bool recoveryPointDrawerOpen: false
    property bool addPanelOpen: false
    readonly property string selectedId: serviceClient.selectedRepositoryConnectionId
    readonly property bool repositorySelected: selectedId.length > 0
    property var selectedRecoveryPointSet: ({})
    property int selectedRecoveryPointCount: 0
    property bool recoveryPointDeleteMode: false
    property bool recoveryPointVerifyMode: false
    /// Boot Check: any number of volume_set recovery points, one StartBootCheck (kind 54) each.
    property bool recoveryPointBootCheckMode: false
    readonly property bool recoveryPointSelectionMode: recoveryPointDeleteMode || recoveryPointVerifyMode
                                                       || recoveryPointBootCheckMode
    property var expandedBackupSetSet: ({})
    property int expandedBackupSetEpoch: 0
    readonly property var backupSetGroups: {
        var _c = serviceClient.recoveryPointCount
        var _l = serviceClient.repositoryLoading
        if (!serviceClient.recoveryPoints)
            return []
        return serviceClient.recoveryPoints.backupSets()
    }
    readonly property string selectedRecoveryPointSummary: {
        var n = root.selectedRecoveryPointCount
        if (n <= 0)
            return ""
        if (n > 1)
            //% "%1 selected"
            return qsTrId("aegra.repository.delete.selected_count").arg(n)
        var id = Object.keys(root.selectedRecoveryPointSet)[0]
        var details = serviceClient.recoveryPoints
                      ? serviceClient.recoveryPoints.recoveryPointDetails(id)
                      : null
        if (!details)
            return ""
        return details.backupTypeText + " · " + details.createdText
    }
    /// Bumped on refresh so free/used volume stats rebind.
    property int storageStatsEpoch: 0
    /// connectionId → recovery point count (filled when that connection catalog loads).
    property var recoveryPointCountById: ({})
    property int recoveryPointCountEpoch: 0

    readonly property int repositoryCount: serviceClient.connections
                                           ? serviceClient.connections.count : 0
    readonly property string usedSpaceText: {
        var _ = root.storageStatsEpoch
        var rp = serviceClient.recoveryPoints
        var _rp = rp ? rp.count : 0
        return serviceClient.formatBytes(serviceClient.repositoryHostUsedBytes())
    }
    readonly property string freeSpaceText: {
        var _ = root.storageStatsEpoch
        var _c = root.repositoryCount
        return serviceClient.formatBytes(serviceClient.repositoryHostFreeBytes())
    }

    function openAddRepositoryPanel() {
        root.addPanelOpen = true
    }

    function refreshStorageStats() {
        root.storageStatsEpoch++
    }

    function rememberRecoveryPointCount() {
        var id = root.selectedId || ""
        if (id.length === 0)
            return
        var next = Object.assign({}, root.recoveryPointCountById)
        next[id] = serviceClient.recoveryPointCount || 0
        root.recoveryPointCountById = next
        root.recoveryPointCountEpoch++
    }

    function recoveryPointCountFor(connectionId) {
        var _ = root.recoveryPointCountEpoch
        if (!connectionId || connectionId.length === 0)
            return 0
        if (connectionId === root.selectedId)
            return serviceClient.recoveryPointCount || 0
        var cached = root.recoveryPointCountById[connectionId]
        return (cached === undefined || cached === null) ? 0 : cached
    }

    function clearRecoveryPointSelection() {
        root.selectedRecoveryPointSet = ({})
        root.selectedRecoveryPointCount = 0
    }

    function exitRecoveryPointDeleteMode() {
        root.recoveryPointDeleteMode = false
        root.recoveryPointVerifyMode = false
        root.recoveryPointBootCheckMode = false
        root.clearRecoveryPointSelection()
    }

    function expandAllBackupSets() {
        var groups = root.backupSetGroups
        var next = Object.assign({}, root.expandedBackupSetSet)
        for (var i = 0; i < groups.length; ++i)
            next[groups[i].backupSetUuid] = true
        root.expandedBackupSetSet = next
        root.expandedBackupSetEpoch++
    }

    function isBootCheckCandidate(fileUuid) {
        if (!fileUuid || !serviceClient.recoveryPoints)
            return false
        var details = serviceClient.recoveryPoints.recoveryPointDetails(fileUuid)
        return !!details && Number(details.contentKind) === 1
    }

    /// Boot check selection is per point (no chain expansion) and only volume_set.
    function toggleBootCheckCandidate(fileUuid) {
        if (!root.isBootCheckCandidate(fileUuid))
            return
        root.applyFileUuidSelection([fileUuid], !root.selectedRecoveryPointSet[fileUuid])
    }

    function bootCheckCandidates(ids) {
        var out = []
        for (var i = 0; i < ids.length; ++i) {
            if (root.isBootCheckCandidate(ids[i]))
                out.push(ids[i])
        }
        return out
    }

    function startSelectedBootCheck() {
        var ids = Object.keys(root.selectedRecoveryPointSet)
        if (ids.length === 0)
            return
        if (serviceClient.bootCheckRecoveryPoints(ids))
            root.exitRecoveryPointDeleteMode()
        else
            //% "Boot check could not be submitted."
            serviceClient.showToast(qsTrId("aegra.repository.bootcheck.submission_failed"), true)
    }

    function clearBackupSetExpansion() {
        root.expandedBackupSetSet = ({})
        root.expandedBackupSetEpoch++
    }

    onSelectedIdChanged: {
        root.exitRecoveryPointDeleteMode()
        root.clearBackupSetExpansion()
    }

    function isRecoveryPointSelected(fileUuid) {
        var _ = root.selectedRecoveryPointCount
        return !!(fileUuid && root.selectedRecoveryPointSet[fileUuid])
    }

    function isBackupSetExpanded(backupSetUuid) {
        var _ = root.expandedBackupSetEpoch
        if (!backupSetUuid)
            return false
        return root.expandedBackupSetSet[backupSetUuid] === true
    }

    function toggleBackupSetExpanded(backupSetUuid) {
        if (!backupSetUuid)
            return
        var next = Object.assign({}, root.expandedBackupSetSet)
        next[backupSetUuid] = !root.isBackupSetExpanded(backupSetUuid)
        root.expandedBackupSetSet = next
        root.expandedBackupSetEpoch++
    }

    function applyFileUuidSelection(ids, selecting) {
        var next = Object.assign({}, root.selectedRecoveryPointSet)
        var i
        for (i = 0; i < ids.length; ++i) {
            if (selecting)
                next[ids[i]] = true
            else
                delete next[ids[i]]
        }
        root.selectedRecoveryPointSet = next
        root.selectedRecoveryPointCount = Object.keys(next).length
    }

    function toggleRecoveryPoint(fileUuid) {
        if (root.recoveryPointBootCheckMode) {
            root.toggleBootCheckCandidate(fileUuid)
            return
        }
        if (!root.recoveryPointDeleteMode)
            return
        if (!fileUuid || fileUuid.length === 0 || !serviceClient.recoveryPoints)
            return
        var descendants = serviceClient.recoveryPoints.descendantFileUuids(fileUuid)
        if (!descendants || descendants.length === 0)
            descendants = [fileUuid]
        var selecting = !root.selectedRecoveryPointSet[fileUuid]
        if (selecting) {
            root.applyFileUuidSelection(descendants, true)
            return
        }
        var ancestors = serviceClient.recoveryPoints.ancestorFileUuids(fileUuid)
        root.applyFileUuidSelection(descendants.concat(ancestors), false)
    }

    function backupSetSelectedCount(backupSetUuid) {
        var _ = root.selectedRecoveryPointCount
        if (!serviceClient.recoveryPoints || !backupSetUuid)
            return 0
        var ids = serviceClient.recoveryPoints.fileUuidsInSet(backupSetUuid)
        var n = 0
        var i
        for (i = 0; i < ids.length; ++i) {
            if (root.selectedRecoveryPointSet[ids[i]])
                ++n
        }
        return n
    }

    function toggleBackupSetSelection(backupSetUuid) {
        if (!root.recoveryPointSelectionMode || serviceClient.repositoryCommandBusy)
            return
        if (!serviceClient.recoveryPoints || !backupSetUuid)
            return
        var ids = serviceClient.recoveryPoints.fileUuidsInSet(backupSetUuid)
        if (root.recoveryPointBootCheckMode)
            ids = root.bootCheckCandidates(ids)
        if (!ids || ids.length === 0)
            return
        var selected = root.backupSetSelectedCount(backupSetUuid)
        root.applyFileUuidSelection(ids, selected !== ids.length)
    }

    function toggleSelectAllRecoveryPoints() {
        if (!root.recoveryPointSelectionMode || serviceClient.repositoryCommandBusy)
            return
        if (!serviceClient.recoveryPoints)
            return
        var ids = serviceClient.recoveryPoints.fileUuids()
        if (root.recoveryPointBootCheckMode)
            ids = root.bootCheckCandidates(ids)
        if (root.selectedRecoveryPointCount > 0
                && root.selectedRecoveryPointCount === ids.length) {
            root.clearRecoveryPointSelection()
            return
        }
        var next = {}
        for (var i = 0; i < ids.length; ++i)
            next[ids[i]] = true
        root.selectedRecoveryPointSet = next
        root.selectedRecoveryPointCount = ids.length
    }

    function requestDeletePlan() {
        var ids = Object.keys(root.selectedRecoveryPointSet)
        if (!ids || ids.length === 0)
            return
        if (!serviceClient.planDeleteRecoveryPoints(ids)) {
            serviceClient.showToast(qsTrId("aegra.repository.delete.plan_failed"), true)
        }
    }

    function confirmExecuteDelete() {
        if (!serviceClient.executeDeletePlan()) {
            serviceClient.showToast(qsTrId("aegra.repository.delete.execute_failed"), true)
        }
    }

    property string pendingRemoveConnectionId: ""

    function requestRemoveConnection(connectionId) {
        root.pendingRemoveConnectionId = connectionId || ""
        if (root.pendingRemoveConnectionId.length === 0)
            return
        removeDialog.open()
    }

    Connections {
        target: serviceClient
        function onDeletePlanReady() {
            deletePlanDialog.open()
        }
        function onDeletePlanFailed(message) {
            serviceClient.showToast(message, true)
        }
        function onDeleteExecuted() {
            root.exitRecoveryPointDeleteMode()
            //% "Recovery points deleted"
            serviceClient.showToast(qsTrId("aegra.repository.delete.done"), false)
            root.refreshStorageStats()
        }
        function onRepositoryChanged() {
            root.rememberRecoveryPointCount()
            root.refreshStorageStats()
        }
        function onRepositoryCommandChanged() {
            // Refresh/Test probe failures are shown per-row (warning + tooltip), not as
            // a global toast — toast cannot identify which repository failed.
            if (!serviceClient.repositoryCommandBusy) {
                root.refreshStorageStats()
            }
        }
    }

    // Staggered entrance: Stage 1 stat cards + actions, Stage 2 table (Event Log style)
    ParallelAnimation {
        id: pageEntranceAnim

        ParallelAnimation {
            NumberAnimation { target: statCard1; property: "opacity"; from: 0; to: 1; duration: 380; easing.type: Easing.OutCubic }
            NumberAnimation { target: statCard1; property: "scale"; from: 0.95; to: 1.0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
            NumberAnimation { target: statCardTrans1; property: "y"; from: 36; to: 0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }

            NumberAnimation { target: statCard2; property: "opacity"; from: 0; to: 1; duration: 420; easing.type: Easing.OutCubic }
            NumberAnimation { target: statCard2; property: "scale"; from: 0.95; to: 1.0; duration: 540; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
            NumberAnimation { target: statCardTrans2; property: "y"; from: 36; to: 0; duration: 540; easing.type: Easing.OutBack; easing.overshoot: 1.25 }

            NumberAnimation { target: statCard3; property: "opacity"; from: 0; to: 1; duration: 460; easing.type: Easing.OutCubic }
            NumberAnimation { target: statCard3; property: "scale"; from: 0.95; to: 1.0; duration: 560; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
            NumberAnimation { target: statCardTrans3; property: "y"; from: 36; to: 0; duration: 560; easing.type: Easing.OutBack; easing.overshoot: 1.25 }

            NumberAnimation { target: iconBox1; property: "scale"; from: 0.2; to: 1.0; duration: 650; easing.type: Easing.OutBack; easing.overshoot: 1.8 }
            NumberAnimation { target: iconBox1; property: "rotation"; from: -25; to: 0; duration: 650; easing.type: Easing.OutBack; easing.overshoot: 1.8 }
            NumberAnimation { target: iconBox2; property: "scale"; from: 0.2; to: 1.0; duration: 680; easing.type: Easing.OutBack; easing.overshoot: 1.8 }
            NumberAnimation { target: iconBox2; property: "rotation"; from: -25; to: 0; duration: 680; easing.type: Easing.OutBack; easing.overshoot: 1.8 }
            NumberAnimation { target: iconBox3; property: "scale"; from: 0.2; to: 1.0; duration: 710; easing.type: Easing.OutBack; easing.overshoot: 1.8 }
            NumberAnimation { target: iconBox3; property: "rotation"; from: -25; to: 0; duration: 710; easing.type: Easing.OutBack; easing.overshoot: 1.8 }
        }

        SequentialAnimation {
            PauseAnimation { duration: 120 }
            ParallelAnimation {
                NumberAnimation { target: repositoryCard; property: "opacity"; from: 0; to: 1; duration: 380; easing.type: Easing.OutCubic }
                NumberAnimation { target: repositoryCard; property: "scale"; from: 0.95; to: 1.0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
                NumberAnimation { target: repositoryCardTrans; property: "y"; from: 36; to: 0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
            }
        }
    }

    function restartEntranceAnimation() {
        pageEntranceAnim.stop()
        statCard1.opacity = 0
        statCard1.scale = 0.95
        statCardTrans1.y = 36
        statCard2.opacity = 0
        statCard2.scale = 0.95
        statCardTrans2.y = 36
        statCard3.opacity = 0
        statCard3.scale = 0.95
        statCardTrans3.y = 36
        iconBox1.scale = 0.2
        iconBox1.rotation = -25
        iconBox2.scale = 0.2
        iconBox2.rotation = -25
        iconBox3.scale = 0.2
        iconBox3.rotation = -25
        repositoryCard.opacity = 0
        repositoryCard.scale = 0.95
        repositoryCardTrans.y = 36
        pageEntranceAnim.restart()
    }

    Component.onCompleted: {
        root.rememberRecoveryPointCount()
        root.refreshStorageStats()
        root.restartEntranceAnimation()
    }

    onVisibleChanged: {
        if (visible)
            root.restartEntranceAnimation()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        // Top stat cards (Backup / Event Log style)
        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Card {
                id: statCard1
                Layout.fillWidth: true
                implicitHeight: 92
                opacity: 0
                scale: 0.95
                transform: Translate { id: statCardTrans1; y: 36 }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 14
                    Rectangle {
                        id: iconBox1
                        width: 44
                        height: 44
                        radius: 14
                        Layout.alignment: Qt.AlignVCenter
                        transformOrigin: Item.Center
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#3B82F6" }
                            GradientStop { position: 1.0; color: "#2563EB" }
                        }
                        Text {
                            anchors.centerIn: parent
                            text: "\uD83D\uDDC4"
                            font.pixelSize: 20
                        }
                    }
                    ColumnLayout {
                        spacing: 2
                        Layout.fillWidth: true
                        Text {
                            //% "Repositories"
                            text: qsTrId("aegra.repository.stat.count")
                            color: Theme.colorTextGrey
                            font.pixelSize: 10
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            //% "%1 repositories"
                            text: qsTrId("aegra.repository.stat.count_value")
                                  .arg(root.repositoryCount)
                            color: Theme.colorTextWhite
                            font.pixelSize: 20
                            font.bold: true
                            font.family: Theme.fontFamily
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Card {
                id: statCard2
                Layout.fillWidth: true
                implicitHeight: 92
                opacity: 0
                scale: 0.95
                transform: Translate { id: statCardTrans2; y: 36 }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 14
                    Rectangle {
                        id: iconBox2
                        width: 44
                        height: 44
                        radius: 14
                        Layout.alignment: Qt.AlignVCenter
                        transformOrigin: Item.Center
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#10B981" }
                            GradientStop { position: 1.0; color: "#059669" }
                        }
                        Text {
                            anchors.centerIn: parent
                            text: "\uD83D\uDCBE"
                            font.pixelSize: 20
                        }
                    }
                    ColumnLayout {
                        spacing: 2
                        Layout.fillWidth: true
                        Text {
                            //% "Used space"
                            text: qsTrId("aegra.repository.stat.used")
                            color: Theme.colorTextGrey
                            font.pixelSize: 10
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            text: root.usedSpaceText
                            color: Theme.colorTextWhite
                            font.pixelSize: 20
                            font.bold: true
                            font.family: Theme.fontFamily
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Card {
                id: statCard3
                Layout.fillWidth: true
                implicitHeight: 92
                opacity: 0
                scale: 0.95
                transform: Translate { id: statCardTrans3; y: 36 }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 14
                    Rectangle {
                        id: iconBox3
                        width: 44
                        height: 44
                        radius: 14
                        Layout.alignment: Qt.AlignVCenter
                        transformOrigin: Item.Center
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#8B5CF6" }
                            GradientStop { position: 1.0; color: "#7C3AED" }
                        }
                        Text {
                            anchors.centerIn: parent
                            text: "\uD83D\uDCCA"
                            font.pixelSize: 20
                        }
                    }
                    ColumnLayout {
                        spacing: 2
                        Layout.fillWidth: true
                        Text {
                            //% "Free space"
                            text: qsTrId("aegra.repository.stat.free")
                            color: Theme.colorTextGrey
                            font.pixelSize: 10
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            text: root.freeSpaceText
                            color: Theme.colorTextWhite
                            font.pixelSize: 20
                            font.bold: true
                            font.family: Theme.fontFamily
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        // Repository table (same standings-style as Backup Schedule)
        Card {
            id: repositoryCard
            Layout.fillWidth: true
            Layout.fillHeight: true
            //% "Repositories"
            title: qsTrId("aegra.repository.stat.count")
            opacity: 0
            scale: 0.95
            transform: Translate { id: repositoryCardTrans; y: 36 }
            headerRightComponent: Component {
                Row {
                    spacing: 8
                    AppButton {
                        //% "Refresh"
                        //% "Reconnect"
                        text: serviceClient.connected
                              ? qsTrId("aegra.common.refresh")
                              : qsTrId("aegra.common.reconnect")
                        enabled: !serviceClient.repositoryLoading
                                 && !serviceClient.repositoryRefreshRunning
                        onClicked: {
                            if (serviceClient.connected) {
                                serviceClient.refreshRepositoryConnections()
                                root.refreshStorageStats()
                            } else {
                                serviceClient.reconnect()
                            }
                        }
                    }
                    AppButton {
                        //% "Add"
                        text: qsTrId("aegra.common.add")
                        primary: true
                        enabled: serviceClient.connected && !serviceClient.repositoryCommandBusy
                                 && !serviceClient.repositoryRefreshRunning
                        onClicked: root.openAddRepositoryPanel()
                    }
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 52
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                anchors.bottomMargin: 10
                spacing: 0

                // Shared column metrics so header and rows stay locked together.
                readonly property int colIndex: 28
                readonly property int colStatus: 90
                readonly property int colFree: 100
                readonly property int colDefault: 80
                readonly property int colRp: 100
                readonly property int colActions: 72
                readonly property int colGap: 4
                readonly property int colHPad: 10

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 48
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        visible: !serviceClient.connections
                                 || serviceClient.connections.count === 0
                        text: {
                            if (serviceClient.connectionsLoading)
                                return qsTrId("aegra.common.loading")
                            if (!serviceClient.connected)
                                return serviceClient.statusText
                            if (serviceClient.connectionsErrorText.length > 0)
                                return serviceClient.connectionsErrorText
                            return qsTrId("aegra.repository.empty")
                        }
                        color: serviceClient.connectionsErrorText.length > 0
                               ? Theme.colorAccentRed : Theme.colorTextGrey
                        font.pixelSize: 13
                        font.family: Theme.fontFamily
                        z: 1
                    }

                    ListView {
                        id: repositoryTable
                        anchors.fill: parent
                        // Always reserve scrollbar gutter so header/rows share the same width.
                        anchors.rightMargin: 10
                        clip: true
                        spacing: 0
                        visible: serviceClient.connections
                                 && serviceClient.connections.count > 0
                        model: serviceClient.connections
                        boundsBehavior: Flickable.StopAtBounds
                        readonly property bool needsScroll: contentHeight > height + 1
                        ScrollBar.vertical: ScrollBar {
                            policy: repositoryTable.needsScroll ? ScrollBar.AlwaysOn
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

                        header: Item {
                            width: repositoryTable.width
                            height: 34
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 4

                                Text {
                                    Layout.preferredWidth: 28
                                    Layout.minimumWidth: 28
                                    Layout.maximumWidth: 28
                                    text: "#"
                                    color: Theme.colorTextDim
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.letterSpacing: 0.8
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                Text {
                                    Layout.preferredWidth: 100
                                    Layout.minimumWidth: 80
                                    Layout.maximumWidth: 160
                                    //% "NAME"
                                    text: qsTrId("aegra.repository.column.name")
                                    color: Theme.colorTextDim
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.letterSpacing: 0.8
                                    font.family: Theme.fontFamily
                                }
                                Item {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 140
                                    Layout.fillHeight: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                    Text {
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        //% "PATH"
                                        text: qsTrId("aegra.repository.column.path")
                                        color: Theme.colorTextDim
                                        font.pixelSize: 11
                                        font.bold: true
                                        font.letterSpacing: 0.8
                                        font.family: Theme.fontFamily
                                        horizontalAlignment: Text.AlignLeft
                                    }
                                }
                                Text {
                                    Layout.preferredWidth: 110
                                    Layout.minimumWidth: 110
                                    Layout.maximumWidth: 110
                                    //% "STATUS"
                                    text: qsTrId("aegra.repository.column.status")
                                    color: Theme.colorTextDim
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.letterSpacing: 0.8
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                Text {
                                    Layout.preferredWidth: 100
                                    Layout.minimumWidth: 100
                                    Layout.maximumWidth: 100
                                    //% "FREE SPACE"
                                    text: qsTrId("aegra.repository.column.free_space")
                                    color: Theme.colorTextDim
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.letterSpacing: 0.8
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                Text {
                                    Layout.preferredWidth: 80
                                    Layout.minimumWidth: 80
                                    Layout.maximumWidth: 80
                                    //% "DEFAULT"
                                    text: qsTrId("aegra.repository.column.default")
                                    color: Theme.colorTextDim
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.letterSpacing: 0.8
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                Text {
                                    Layout.preferredWidth: 100
                                    Layout.minimumWidth: 100
                                    Layout.maximumWidth: 100
                                    //% "RECOVERY POINTS"
                                    text: qsTrId("aegra.repository.column.recovery_points")
                                    color: Theme.colorTextDim
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.letterSpacing: 0.8
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                Item {
                                    Layout.preferredWidth: 40
                                    Layout.minimumWidth: 40
                                    Layout.maximumWidth: 40
                                }
                            }
                        }

                        delegate: Item {
                            id: repoRow
                            required property string connectionId
                            required property string displayName
                            required property string locator
                            required property string stateText
                            required property bool isDefault
                            required property bool isAvailable
                            required property bool isRefreshing
                            required property string probeErrorText
                            required property int index
                            width: repositoryTable.width
                            height: 52

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                height: 1
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0.00; color: "transparent" }
                                    GradientStop { position: 0.15; color: Theme.colorBorder }
                                    GradientStop { position: 0.85; color: Theme.colorBorder }
                                    GradientStop { position: 1.00; color: "transparent" }
                                }
                            }

                            HoverHandler {
                                id: rowHover
                                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                            }

                            Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: 4
                                anchors.rightMargin: 4
                                radius: 10
                                color: rowHover.hovered ? Theme.colorHover : "transparent"
                            }

                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton
                                enabled: !serviceClient.repositoryRefreshRunning
                                onClicked: serviceClient.selectRepositoryConnection(connectionId)
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 4

                                Text {
                                    Layout.preferredWidth: 28
                                    Layout.minimumWidth: 28
                                    Layout.maximumWidth: 28
                                    text: "" + (repoRow.index + 1)
                                    color: Theme.colorTextDim
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }

                                Text {
                                    Layout.preferredWidth: 100
                                    Layout.minimumWidth: 80
                                    Layout.maximumWidth: 160
                                    Layout.fillHeight: true
                                    text: displayName
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 14
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }

                                Item {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 140
                                    Layout.fillHeight: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                    Text {
                                        id: pathText
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: locator || ""
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                        elide: Text.ElideMiddle
                                        horizontalAlignment: Text.AlignLeft
                                        verticalAlignment: Text.AlignVCenter
                                        ToolTip.visible: truncated && pathHover.hovered
                                        ToolTip.delay: 400
                                        ToolTip.text: locator || ""

                                        HoverHandler {
                                            id: pathHover
                                            acceptedDevices: PointerDevice.Mouse
                                                             | PointerDevice.TouchPad
                                        }
                                    }
                                }

                                // Status: Online/Offline (+ warning on probe fail), or spinning while refreshing.
                                Item {
                                    Layout.preferredWidth: 110
                                    Layout.minimumWidth: 110
                                    Layout.maximumWidth: 110
                                    Layout.fillHeight: true

                                    Row {
                                        anchors.centerIn: parent
                                        spacing: 6
                                        visible: !isRefreshing

                                        // Status icon (Online check-circle / Offline warning-triangle)
                                        Canvas {
                                            id: statusIconCanvas
                                            width: 18
                                            height: 18
                                            anchors.verticalCenter: parent.verticalCenter
                                            antialiasing: true
                                            renderTarget: Canvas.FramebufferObject
                                            renderStrategy: Canvas.Cooperative

                                            property bool available: isAvailable
                                            property color greenColor: Theme.colorGreen
                                            property color redColor: Theme.colorAccentRed

                                            onAvailableChanged: requestPaint()
                                            onGreenColorChanged: requestPaint()
                                            onRedColorChanged: requestPaint()
                                            Component.onCompleted: requestPaint()

                                            onPaint: {
                                                var ctx = getContext("2d")
                                                ctx.reset()
                                                ctx.clearRect(0, 0, width, height)
                                                var cx = width / 2
                                                var cy = height / 2

                                                if (available) {
                                                    // Online: Green check-circle icon
                                                    ctx.strokeStyle = Theme.colorGreen
                                                    ctx.lineWidth = 1.8
                                                    ctx.lineCap = "round"
                                                    ctx.lineJoin = "round"

                                                    // Circle outline
                                                    ctx.beginPath()
                                                    ctx.arc(cx, cy, 7.0, 0, Math.PI * 2)
                                                    ctx.stroke()

                                                    // Checkmark
                                                    ctx.beginPath()
                                                    ctx.moveTo(cx - 3.8, cy - 0.2)
                                                    ctx.lineTo(cx - 1.0, cy + 2.6)
                                                    ctx.lineTo(cx + 4.0, cy - 2.8)
                                                    ctx.stroke()
                                                } else {
                                                    // Offline: Red warning / alert triangle icon
                                                    var color = Theme.colorAccentRed
                                                    ctx.strokeStyle = color
                                                    ctx.fillStyle = color
                                                    ctx.lineWidth = 1.8
                                                    ctx.lineCap = "round"
                                                    ctx.lineJoin = "round"

                                                    // Warning triangle
                                                    ctx.beginPath()
                                                    ctx.moveTo(cx, cy - 7.0)
                                                    ctx.lineTo(cx + 7.5, cy + 6.5)
                                                    ctx.lineTo(cx - 7.5, cy + 6.5)
                                                    ctx.closePath()
                                                    ctx.stroke()

                                                    // Exclamation mark line & dot
                                                    ctx.beginPath()
                                                    ctx.moveTo(cx, cy - 2.8)
                                                    ctx.lineTo(cx, cy + 1.6)
                                                    ctx.stroke()

                                                    ctx.beginPath()
                                                    ctx.arc(cx, cy + 4.2, 1.0, 0, Math.PI * 2)
                                                    ctx.fill()
                                                }
                                            }
                                        }

                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: stateText
                                            color: isAvailable ? Theme.colorGreen : Theme.colorAccentRed
                                            font.pixelSize: 13
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                        }

                                        ToolTip.visible: !isAvailable && probeErrorText && probeErrorText.length > 0 && statusWarnHover.hovered
                                        ToolTip.delay: 300
                                        ToolTip.text: probeErrorText || ""

                                        HoverHandler {
                                            id: statusWarnHover
                                            acceptedDevices: PointerDevice.Mouse
                                                             | PointerDevice.TouchPad
                                        }
                                    }

                                    Item {
                                        id: refreshStatusIcon
                                        anchors.centerIn: parent
                                        width: 18
                                        height: 18
                                        visible: isRefreshing
                                        property real spinAngle: 0
                                        rotation: spinAngle

                                        // Dual circular arrows (sync)
                                        Canvas {
                                            id: refreshSyncCanvas
                                            anchors.fill: parent
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
                                                ctx.lineWidth = 1.8
                                                ctx.lineCap = "round"
                                                ctx.lineJoin = "round"

                                                var cx = width / 2
                                                var cy = height / 2
                                                var r = width / 2 - 2.0

                                                function drawArcArrow(startAng, endAng) {
                                                    ctx.beginPath()
                                                    ctx.arc(cx, cy, r, startAng, endAng, false)
                                                    ctx.stroke()
                                                    var tipX = cx + Math.cos(endAng) * r
                                                    var tipY = cy + Math.sin(endAng) * r
                                                    var tang = endAng + Math.PI / 2
                                                    var hx = Math.cos(tang)
                                                    var hy = Math.sin(tang)
                                                    var nx = Math.cos(endAng)
                                                    var ny = Math.sin(endAng)
                                                    var len = 3.6
                                                    var wing = 2.4
                                                    ctx.beginPath()
                                                    ctx.moveTo(tipX + hx * len, tipY + hy * len)
                                                    ctx.lineTo(tipX - nx * wing - hx * 0.4,
                                                               tipY - ny * wing - hy * 0.4)
                                                    ctx.lineTo(tipX + nx * wing - hx * 0.4,
                                                               tipY + ny * wing - hy * 0.4)
                                                    ctx.closePath()
                                                    ctx.fill()
                                                }

                                                drawArcArrow(-Math.PI * 0.85, -Math.PI * 0.15)
                                                drawArcArrow(Math.PI * 0.15, Math.PI * 0.85)
                                            }
                                        }

                                        NumberAnimation on spinAngle {
                                            running: isRefreshing
                                            from: 0
                                            to: 360
                                            loops: Animation.Infinite
                                            duration: 1200
                                        }
                                    }
                                }

                                Text {
                                    Layout.preferredWidth: 100
                                    Layout.minimumWidth: 100
                                    Layout.maximumWidth: 100
                                    text: {
                                        var _ = root.storageStatsEpoch
                                        return serviceClient.freeSpaceTextForLocator(locator || "")
                                    }
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                }

                                // DEFAULT: badge when default, otherwise star to set default.
                                Item {
                                    Layout.preferredWidth: 80
                                    Layout.minimumWidth: 80
                                    Layout.maximumWidth: 80
                                    Layout.fillHeight: true

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: defLab.implicitWidth + 14
                                        height: 22
                                        radius: 3
                                        visible: isDefault
                                        color: Theme.colorButton
                                        border.width: 1
                                        border.color: Theme.colorBorder
                                        Text {
                                            id: defLab
                                            anchors.centerIn: parent
                                            //% "Default"
                                            text: qsTrId("aegra.backup.connection.default")
                                            color: Theme.colorTextWhite
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 11
                                            font.bold: true
                                        }
                                    }

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 28
                                        height: 28
                                        radius: 8
                                        visible: !isDefault
                                        color: starHover.containsMouse
                                               ? Theme.colorButtonHover : Theme.colorButton
                                        opacity: serviceClient.connected
                                                 && !serviceClient.repositoryCommandBusy
                                                 && !serviceClient.repositoryRefreshRunning
                                                 ? 1.0 : 0.45
                                        Text {
                                            anchors.centerIn: parent
                                            text: "\u2605"
                                            color: Theme.colorAccentBlue
                                            font.pixelSize: 13
                                        }
                                        MouseArea {
                                            id: starHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            enabled: serviceClient.connected
                                                     && !serviceClient.repositoryCommandBusy
                                                     && !serviceClient.repositoryRefreshRunning
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: serviceClient.setDefaultRepositoryConnection(
                                                           connectionId)
                                        }
                                    }
                                }

                                Item {
                                    Layout.preferredWidth: 100
                                    Layout.minimumWidth: 100
                                    Layout.maximumWidth: 100
                                    Layout.fillHeight: true
                                    AppButton {
                                        anchors.centerIn: parent
                                        implicitWidth: 56
                                        text: "" + root.recoveryPointCountFor(connectionId)
                                        enabled: serviceClient.connected
                                                 && !serviceClient.repositoryLoading
                                        onClicked: {
                                            serviceClient.selectRepositoryConnection(connectionId)
                                            root.recoveryPointDrawerOpen = true
                                        }
                                    }
                                }

                                // Actions — delete only (set-default star lives in DEFAULT column).
                                Item {
                                    Layout.preferredWidth: 40
                                    Layout.minimumWidth: 40
                                    Layout.maximumWidth: 40
                                    Layout.fillHeight: true

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 28
                                        height: 28
                                        radius: 8
                                        // Hidden until the row is hovered (keeps table clean).
                                        visible: rowHover.hovered
                                        color: delHover.containsMouse ? "#e03333" : "#cc3333"
                                        opacity: serviceClient.connected
                                                 && !serviceClient.repositoryCommandBusy
                                                 && !serviceClient.repositoryRefreshRunning
                                                 ? 1.0 : 0.45
                                        Item {
                                            anchors.centerIn: parent
                                            width: 12
                                            height: 13
                                            Rectangle {
                                                x: 1; y: 0; width: 10; height: 2; radius: 1
                                                color: "#ffffff"
                                            }
                                            Rectangle {
                                                x: 3; y: 2; width: 6; height: 2
                                                color: "#ffffff"
                                            }
                                            Rectangle {
                                                x: 2; y: 4; width: 8; height: 9; radius: 1
                                                color: "#ffffff"
                                            }
                                            Rectangle {
                                                x: 3.5; y: 6; width: 1.2; height: 5
                                                color: "#cc3333"
                                            }
                                            Rectangle {
                                                x: 5.5; y: 6; width: 1.2; height: 5
                                                color: "#cc3333"
                                            }
                                            Rectangle {
                                                x: 7.5; y: 6; width: 1.2; height: 5
                                                color: "#cc3333"
                                            }
                                        }
                                        MouseArea {
                                            id: delHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            enabled: serviceClient.connected
                                                     && !serviceClient.repositoryCommandBusy
                                                     && !serviceClient.repositoryRefreshRunning
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.requestRemoveConnection(connectionId)
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

    // Add repository — 90% right slide-in drawer (old RepositoryPage baseline).
    Item {
        id: addRepositoryDrawer
        anchors.fill: parent
        z: 2200
        enabled: root.addPanelOpen || addDrawerPanel.slideProgress < 0.999

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusWindow
            color: Theme.colorScrim
            opacity: root.addPanelOpen ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 250 } }
            MouseArea {
                anchors.fill: parent
                enabled: root.addPanelOpen
                // Close only via Cancel / ✕ (match old panel).
            }
        }

        Rectangle {
            id: addDrawerPanel
            width: Math.max(560, parent.width * 0.9)
            height: parent.height
            y: 0
            property real slideProgress: root.addPanelOpen ? 0 : 1
            x: parent.width - width + slideProgress * width
            visible: slideProgress < 0.999 || root.addPanelOpen
            color: Theme.colorBg
            radius: Theme.radiusWindow
            clip: true
            border.width: 1
            border.color: Theme.colorBorder

            Behavior on slideProgress {
                NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    spacing: 8

                    Rectangle {
                        Layout.preferredWidth: 3
                        Layout.preferredHeight: 18
                        color: Theme.colorAccentBlue
                    }

                    Text {
                        Layout.fillWidth: true
                        //% "Add repository"
                        text: qsTrId("aegra.repository.add")
                        color: Theme.colorTextWhite
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.bold: true
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
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        enabled: !addRepoPanel.isSubmitting
                        onClicked: root.addPanelOpen = false
                    }
                }

                AddRepositoryPanel {
                    id: addRepoPanel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    onFinished: root.addPanelOpen = false
                    onCancelled: root.addPanelOpen = false
                }
            }
        }

        Connections {
            target: root
            function onAddPanelOpenChanged() {
                if (root.addPanelOpen && addRepoPanel)
                    addRepoPanel.activate()
            }
        }
    }

    // Confirm before removing a repository connection (matches Backup delete-schedule chrome).
    Popup {
        id: removeDialog
        parent: Overlay.overlay
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        anchors.centerIn: Overlay.overlay
        width: Math.min(420, Overlay.overlay ? Overlay.overlay.width - 48 : 420)
        padding: 20
        property bool committing: false

        onClosed: {
            if (!removeDialog.committing)
                root.pendingRemoveConnectionId = ""
            removeDialog.committing = false
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
                //% "Remove repository connection?"
                text: qsTrId("aegra.repository.remove_title")
                color: Theme.colorTextWhite
                font.pixelSize: 16
                font.bold: true
                font.family: Theme.fontFamily
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                //% "Only the saved connection is removed. Backup data is not deleted."
                text: qsTrId("aegra.repository.remove_description")
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
                        removeDialog.committing = true
                        root.pendingRemoveConnectionId = ""
                        removeDialog.close()
                    }
                }
                AppButton {
                    //% "Remove"
                    text: qsTrId("aegra.common.remove")
                    primary: true
                    Layout.preferredHeight: 36
                    onClicked: {
                        removeDialog.committing = true
                        var id = root.pendingRemoveConnectionId.length > 0
                                 ? root.pendingRemoveConnectionId : root.selectedId
                        if (id.length > 0)
                            serviceClient.removeRepositoryConnection(id)
                        root.pendingRemoveConnectionId = ""
                        removeDialog.close()
                    }
                }
            }
        }
    }

    Item {
        id: recoveryPointDrawer
        anchors.fill: parent
        z: 2100
        enabled: root.recoveryPointDrawerOpen || drawerPanel.slideProgress < 0.999

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusWindow
            color: Theme.colorScrim
            opacity: root.recoveryPointDrawerOpen ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 250 } }
            MouseArea {
                anchors.fill: parent
                enabled: root.recoveryPointDrawerOpen
            }
        }

        Rectangle {
            id: drawerPanel
            width: Math.max(560, parent.width * 0.9)
            height: parent.height
            y: 0
            property real slideProgress: root.recoveryPointDrawerOpen ? 0 : 1
            x: parent.width - width + slideProgress * width
            visible: slideProgress < 0.999 || root.recoveryPointDrawerOpen
            color: Theme.colorBg
            radius: Theme.radiusWindow
            clip: true
            border.width: 1
            border.color: Theme.colorBorder

            Behavior on slideProgress {
                NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    spacing: 8

                    Rectangle {
                        Layout.preferredWidth: 3
                        Layout.preferredHeight: 18
                        color: Theme.colorAccentBlue
                    }

                    Text {
                        Layout.fillWidth: true
                        //% "Personal Repository — Recovery Points"
                        text: qsTrId("aegra.repository.drawer_title")
                        color: Theme.colorTextWhite
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.bold: true
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
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: {
                            root.exitRecoveryPointDeleteMode()
                            root.recoveryPointDrawerOpen = false
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    AppButton {
                        //% "Refresh"
                        text: qsTrId("aegra.common.refresh")
                        enabled: serviceClient.connected && !serviceClient.repositoryLoading
                        onClicked: serviceClient.refreshRepository()
                    }
                    AppButton {
                        //% "Delete"
                        text: qsTrId("aegra.common.delete")
                        danger: true
                        enabled: serviceClient.connected
                                 && !root.recoveryPointVerifyMode
                                 && !root.recoveryPointBootCheckMode
                                 && !serviceClient.repositoryCommandBusy
                                 && !serviceClient.deletePlanBusy
                                 && !serviceClient.repositoryLoading
                                 && (!root.recoveryPointDeleteMode
                                     || root.selectedRecoveryPointCount > 0)
                        onClicked: {
                            if (!root.recoveryPointDeleteMode) {
                                root.recoveryPointDeleteMode = true
                                return
                            }
                            root.requestDeletePlan()
                        }
                    }
                    AppButton {
                        text: qsTrId("aegra.job.operation.verify")
                        enabled: serviceClient.connected && serviceClient.verifyAvailable()
                                 && !serviceClient.repositoryLoading
                                 && !serviceClient.repositoryCommandBusy
                                 && !root.recoveryPointDeleteMode
                                 && !root.recoveryPointBootCheckMode
                                 && (!root.recoveryPointVerifyMode || root.selectedRecoveryPointCount > 0)
                        onClicked: {
                            if (!root.recoveryPointVerifyMode) {
                                root.clearRecoveryPointSelection()
                                root.recoveryPointVerifyMode = true
                                return
                            }
                            if (serviceClient.verifyRecoveryPoints(Object.keys(root.selectedRecoveryPointSet)))
                                root.exitRecoveryPointDeleteMode()
                            else
                                serviceClient.showToast(qsTrId("aegra.repository.verify.submission_failed"), true)
                        }
                    }
                    AppButton {
                        // First click enters selection mode (volume_set points only);
                        // second click submits one StartBootCheck per selected point.
                        text: qsTrId("aegra.job.operation.bootcheck")
                        enabled: serviceClient.connected && serviceClient.bootCheckAvailable()
                                 && !serviceClient.repositoryLoading
                                 && !serviceClient.repositoryCommandBusy
                                 && !root.recoveryPointDeleteMode
                                 && !root.recoveryPointVerifyMode
                                 && (!root.recoveryPointBootCheckMode || root.selectedRecoveryPointCount > 0)
                        onClicked: {
                            if (!root.recoveryPointBootCheckMode) {
                                root.clearRecoveryPointSelection()
                                root.recoveryPointBootCheckMode = true
                                root.expandAllBackupSets()
                                return
                            }
                            root.startSelectedBootCheck()
                        }
                    }
                    AppButton {
                        //% "Cancel"
                        text: qsTrId("aegra.common.cancel")
                        visible: root.recoveryPointSelectionMode
                        onClicked: root.exitRecoveryPointDeleteMode()
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        visible: root.recoveryPointBootCheckMode && root.selectedRecoveryPointCount === 0
                        //% "Select volume recovery points"
                        text: qsTrId("aegra.repository.bootcheck.select_hint")
                        color: Theme.colorTextGrey
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        Layout.maximumWidth: 260
                    }
                    Text {
                        visible: root.selectedRecoveryPointCount > 0
                        text: root.selectedRecoveryPointSummary
                        color: Theme.colorTextGrey
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        Layout.maximumWidth: 220
                    }
                    Text {
                        //% "%1 recovery points"
                        text: qsTrId("aegra.repository.recovery_points_count")
                              .arg(serviceClient.recoveryPointCount)
                        color: Theme.colorTextDim
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                    }
                }

                Card {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.topMargin: 10
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        anchors.bottomMargin: 10
                        spacing: 0

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 8

                        Item {
                            Layout.preferredWidth: root.recoveryPointSelectionMode ? 28 : 0
                            Layout.fillHeight: true
                            visible: root.recoveryPointSelectionMode
                            Rectangle {
                                width: 16
                                height: 16
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.horizontalCenter: parent.horizontalCenter
                                radius: 3
                                color: root.selectedRecoveryPointCount > 0
                                       ? Theme.colorAccentBlue : Theme.colorInput
                                border.width: 1
                                border.color: root.selectedRecoveryPointCount > 0
                                             ? Theme.colorAccentBlue : Theme.colorTextGrey
                                Text {
                                    anchors.centerIn: parent
                                    visible: root.selectedRecoveryPointCount > 0
                                    text: (serviceClient.recoveryPointCount > 0
                                           && root.selectedRecoveryPointCount
                                              === serviceClient.recoveryPointCount)
                                          ? "\u2713" : "\u2212"
                                    color: "white"
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.toggleSelectAllRecoveryPoints()
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            //% "Recovery point"
                            text: qsTrId("aegra.repository.column.backup_content").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 120
                            //% "Backup time"
                            text: qsTrId("aegra.repository.column.backup_time").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 100
                            //% "Type"
                            text: qsTrId("aegra.repository.column.type").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 80
                            //% "Image size"
                            text: qsTrId("aegra.repository.column.image_size").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 88
                            horizontalAlignment: Text.AlignHCenter
                            //% "Verify"
                            text: qsTrId("aegra.repository.column.verify_status").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 88
                            horizontalAlignment: Text.AlignHCenter
                            //% "Boot check"
                            text: qsTrId("aegra.repository.column.boot_check_status").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                        }
                    }
                }

                ListView {
                    id: recoveryPointList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: root.backupSetGroups
                    spacing: 0

                    delegate: Item {
                        id: backupSetGroup
                        width: recoveryPointList.width
                        height: groupColumn.height
                        property string backupSetUuid: modelData.backupSetUuid || ""
                        property int setPointCount: Number(modelData.recoveryPointCount || 0)
                        property int setSelectedCount: root.backupSetSelectedCount(backupSetUuid)
                        property bool expanded: root.isBackupSetExpanded(backupSetUuid)
                        property var points: expanded && serviceClient.recoveryPoints
                                             ? serviceClient.recoveryPoints.recoveryPointsInSet(
                                                   backupSetUuid)
                                             : []
                        // Status aggregation must not depend on expansion: the row
                        // model above is intentionally empty while collapsed.
                        readonly property var statusFileUuids: {
                            var _c = serviceClient.recoveryPointCount
                            return serviceClient.recoveryPoints
                                   ? serviceClient.recoveryPoints.fileUuidsInSet(backupSetUuid)
                                   : []
                        }

                        Column {
                            id: groupColumn
                            width: backupSetGroup.width

                        Item {
                            width: backupSetGroup.width
                            height: 52

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                height: 1
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0.00; color: "transparent" }
                                    GradientStop { position: 0.15; color: Theme.colorBorder }
                                    GradientStop { position: 0.85; color: Theme.colorBorder }
                                    GradientStop { position: 1.00; color: "transparent" }
                                }
                            }

                            HoverHandler {
                                id: setHover
                                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                            }

                            Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: 4
                                anchors.rightMargin: 4
                                radius: 10
                                color: setHover.hovered ? Theme.colorHover : "transparent"
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 8

                                MouseArea {
                                    Layout.preferredWidth: 16
                                    Layout.fillHeight: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.toggleBackupSetExpanded(
                                                   backupSetGroup.backupSetUuid)
                                    Text {
                                        anchors.centerIn: parent
                                        text: backupSetGroup.expanded ? "\u25BE" : "\u25B8"
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 12
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                                }

                                Rectangle {
                                    visible: root.recoveryPointSelectionMode
                                    Layout.preferredWidth: root.recoveryPointSelectionMode ? 16 : 0
                                    Layout.preferredHeight: 16
                                    radius: 3
                                    color: backupSetGroup.setSelectedCount > 0
                                           ? Theme.colorAccentBlue : Theme.colorInput
                                    border.width: 1
                                    border.color: backupSetGroup.setSelectedCount > 0
                                                 ? Theme.colorAccentBlue : Theme.colorTextGrey
                                    Text {
                                        anchors.centerIn: parent
                                        visible: backupSetGroup.setSelectedCount > 0
                                        text: backupSetGroup.setSelectedCount
                                              === backupSetGroup.setPointCount
                                              ? "\u2713" : "\u2212"
                                        color: "white"
                                        font.pixelSize: 11
                                        font.bold: true
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            mouse.accepted = true
                                            root.toggleBackupSetSelection(
                                                backupSetGroup.backupSetUuid)
                                        }
                                    }
                                }

                                ScheduleTypeIcon {
                                    Layout.preferredWidth: 28
                                    Layout.preferredHeight: 28
                                    size: 28
                                    kind: Number(modelData.contentKind) === 2 ? "files" : "volume"
                                    ink: Theme.colorTextGrey
                                }

                                Column {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Text {
                                        width: parent.width
                                        text: root.backupSetPresentation(modelData).title
                                        color: Theme.colorTextWhite
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 13
                                        font.bold: true
                                        elide: Text.ElideMiddle
                                    }
                                    Text {
                                        width: parent.width
                                        //% "%1 recovery points · latest %2"
                                        text: (root.backupSetPresentation(modelData).source.length > 0
                                               && root.backupSetPresentation(modelData).source !== root.backupSetPresentation(modelData).title
                                               ? root.backupSetPresentation(modelData).source + " · " : "")
                                              + qsTrId("aegra.repository.backup_set.summary")
                                              .arg(backupSetGroup.setPointCount)
                                              .arg(modelData.latestCreatedText || "")
                                        color: Theme.colorTextDim
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }

                                Item { Layout.preferredWidth: 80 }
                                Item {
                                    Layout.preferredWidth: 88
                                    Layout.fillHeight: true
                                    StatusGlyph {
                                        anchors.centerIn: parent
                                        size: 16
                                        kind: {
                                            var _r = root.jobsRevision
                                            return root.checkGlyphKind(
                                                root.backupSetCheckKey(backupSetGroup.statusFileUuids, 3))
                                        }
                                        label: {
                                            var _r = root.jobsRevision
                                            return root.backupSetCheckLabel(backupSetGroup.statusFileUuids, 3)
                                        }
                                    }
                                }
                                Item {
                                    Layout.preferredWidth: 88
                                    Layout.fillHeight: true
                                    StatusGlyph {
                                        anchors.centerIn: parent
                                        size: 16
                                        kind: {
                                            var _r = root.jobsRevision
                                            return root.checkGlyphKind(
                                                root.backupSetCheckKey(backupSetGroup.statusFileUuids, 5))
                                        }
                                        label: {
                                            var _r = root.jobsRevision
                                            return root.backupSetCheckLabel(backupSetGroup.statusFileUuids, 5)
                                        }
                                    }
                                }
                            }

                        }

                        Repeater {
                            model: backupSetGroup.points
                            delegate: Item {
                                width: backupSetGroup.width
                                height: 52
                                property bool selected: root.isRecoveryPointSelected(
                                                            modelData.fileUuid)

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    height: 1
                                    gradient: Gradient {
                                        orientation: Gradient.Horizontal
                                        GradientStop { position: 0.00; color: "transparent" }
                                        GradientStop { position: 0.15; color: Theme.colorBorder }
                                        GradientStop { position: 0.85; color: Theme.colorBorder }
                                        GradientStop { position: 1.00; color: "transparent" }
                                    }
                                }

                                HoverHandler {
                                    id: rpHover
                                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                                }

                                Rectangle {
                                    anchors.fill: parent
                                    anchors.leftMargin: 4
                                    anchors.rightMargin: 4
                                    radius: 10
                                    color: rpHover.hovered ? Theme.colorHover : "transparent"
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    spacing: 8

                                    // One indent step past the backup-set chevron so Full/Inc sit as children.
                                    Item { Layout.preferredWidth: 40 }

                                    Rectangle {
                                        readonly property bool bootCheckSelectable:
                                            root.recoveryPointBootCheckMode
                                            && Number(modelData.contentKind) === 1
                                        readonly property bool pointSelectable:
                                            root.recoveryPointDeleteMode || bootCheckSelectable
                                        visible: root.recoveryPointDeleteMode || root.recoveryPointBootCheckMode
                                        opacity: pointSelectable ? 1 : 0.3
                                        Layout.preferredWidth: visible ? 16 : 0
                                        Layout.preferredHeight: 16
                                        radius: 3
                                        color: selected ? Theme.colorAccentBlue : Theme.colorInput
                                        border.width: 1
                                        border.color: selected ? Theme.colorAccentBlue
                                                               : Theme.colorTextGrey
                                        Text {
                                            anchors.centerIn: parent
                                            visible: selected
                                            text: "\u2713"
                                            color: "white"
                                            font.pixelSize: 11
                                            font.bold: true
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: parent.pointSelectable
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                mouse.accepted = true
                                                root.toggleRecoveryPoint(modelData.fileUuid)
                                            }
                                        }
                                    }

                                    Column {
                                        Layout.fillWidth: true
                                        spacing: 2

                                        Text {
                                            width: parent.width
                                            text: modelData.displayTitle || ""
                                            color: Theme.colorTextWhite
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 13
                                            font.bold: true
                                            elide: Text.ElideMiddle
                                        }
                                        Text {
                                            width: parent.width
                                            text: [modelData.isLatest ? qsTrId("aegra.repository.point.latest") : "",
                                                   modelData.isBaseline ? qsTrId("aegra.repository.point.baseline") : ""]
                                                  .filter(function(label) { return label.length > 0 }).join(" · ")
                                            color: Theme.colorTextDim
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 10
                                            elide: Text.ElideRight
                                        }
                                    }

                                    Text {
                                        Layout.preferredWidth: 120
                                        text: modelData.createdText || ""
                                        color: Theme.colorTextGrey
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 12
                                    }
                                    Text {
                                        Layout.preferredWidth: 100
                                        text: modelData.backupTypeText || ""
                                        color: modelData.isBaseline
                                               ? Theme.colorTextWhite : Theme.colorTextGrey
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 13
                                        font.bold: modelData.isBaseline === true
                                    }
                                    Text {
                                        Layout.preferredWidth: 80
                                        text: modelData.storedSizeText || ""
                                        color: Theme.colorTextGrey
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 12
                                    }
                                    Item {
                                        Layout.preferredWidth: 88
                                        Layout.fillHeight: true
                                        StatusGlyph {
                                            anchors.centerIn: parent
                                            size: 16
                                            kind: {
                                                var _r = root.jobsRevision
                                                var key = root.recoveryPointCheckKey(modelData.fileUuid, 3)
                                                if (key === "na" && modelData.chainComplete !== true)
                                                    return "incomplete"
                                                return root.checkGlyphKind(key)
                                            }
                                            label: {
                                                var _r = root.jobsRevision
                                                var key = root.recoveryPointCheckKey(modelData.fileUuid, 3)
                                                if (key === "na" && modelData.chainComplete !== true)
                                                    return modelData.chainStateText || ""
                                                return root.recoveryPointCheckLabel(modelData.fileUuid, 3)
                                            }
                                        }
                                    }
                                    Item {
                                        Layout.preferredWidth: 88
                                        Layout.fillHeight: true
                                        StatusGlyph {
                                            anchors.centerIn: parent
                                            size: 16
                                            kind: {
                                                var _r = root.jobsRevision
                                                return root.checkGlyphKind(
                                                    root.recoveryPointCheckKey(modelData.fileUuid, 5))
                                            }
                                            label: {
                                                var _r = root.jobsRevision
                                                return root.recoveryPointCheckLabel(modelData.fileUuid, 5)
                                            }
                                        }
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    enabled: !root.recoveryPointSelectionMode
                                    cursorShape: Qt.PointingHandCursor
                                    onDoubleClicked: {
                                        root.pointDetails = modelData
                                        recoveryPointDetailsDialog.open()
                                    }
                                }
                            }
                        }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: recoveryPointList.count === 0 && !serviceClient.repositoryLoading
                        //% "No recovery points"
                        text: qsTrId("aegra.repository.empty_recovery_points")
                        color: Theme.colorTextGrey
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                    }
                }
                    }
                }
            }
        }
    }

    Dialog {
        id: recoveryPointDetailsDialog
        anchors.centerIn: parent
        width: Math.min(520, root.width - 40)
        modal: true
        title: root.pointDetails.displayTitle || ""
        header: Text {
            padding: 16
            text: recoveryPointDetailsDialog.title
            color: Theme.colorTextWhite
            font.family: Theme.fontFamily
            font.bold: true
            wrapMode: Text.Wrap
        }
        background: Rectangle { color: Theme.colorPopup; radius: 8; border.color: Theme.colorBorder }
        contentItem: ColumnLayout {
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: (root.pointDetails.backupTypeText || "") + " · " + (root.pointDetails.createdText || "")
                color: Theme.colorTextWhite
                font.family: Theme.fontFamily
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                text: root.pointDetails.parentSummaryText || ""
                color: Theme.colorTextGrey
                font.family: Theme.fontFamily
                wrapMode: Text.Wrap
            }
            Text {
                text: qsTrId("aegra.repository.point.technical")
                color: Theme.colorTextDim
                font.family: Theme.fontFamily
            }
            Text {
                Layout.fillWidth: true
                text: root.pointDetails.fileUuid || ""
                color: Theme.colorTextGrey
                font.family: Theme.fontFamily
                wrapMode: Text.WrapAnywhere
            }
            RowLayout {
                AppButton {
                    text: qsTrId("aegra.repository.point.copy_id")
                    onClicked: serviceClient.recoveryPoints.copyIdentifier(root.pointDetails.fileUuid)
                }
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTrId("aegra.common.close")
                    onClicked: recoveryPointDetailsDialog.close()
                }
            }
        }
    }

    // Server-authored delete plan confirmation (target count only; no local dependency calc).
    Dialog {
        id: deletePlanDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(440, parent.width - 40)
        //% "Delete recovery points"
        title: qsTrId("aegra.repository.delete.title")
        standardButtons: Dialog.NoButton
        background: Rectangle {
            color: Theme.colorPopup
            border.color: Theme.colorBorder
            radius: 8
        }
        contentItem: ColumnLayout {
            spacing: 14
            width: deletePlanDialog.availableWidth
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                //% "The service planned to delete %1 recovery point(s). Other recovery points are kept. This cannot be undone."
                text: qsTrId("aegra.repository.delete.plan_message")
                      .arg((serviceClient.deletePlan && serviceClient.deletePlan.targetCount)
                           ? serviceClient.deletePlan.targetCount : 0)
                color: Theme.colorTextWhite
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }
            Text {
                Layout.fillWidth: true
                visible: serviceClient.deletePlan
                         && serviceClient.deletePlan.retainedCount !== undefined
                wrapMode: Text.WordWrap
                //% "Approximately %1 other recovery point(s) currently listed will remain."
                text: qsTrId("aegra.repository.delete.retained_hint")
                      .arg((serviceClient.deletePlan
                            && serviceClient.deletePlan.retainedCount !== undefined)
                           ? serviceClient.deletePlan.retainedCount : 0)
                color: Theme.colorTextGrey
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Item { Layout.fillWidth: true }
                AppButton {
                    //% "Cancel"
                    text: qsTrId("aegra.common.cancel")
                    onClicked: {
                        serviceClient.clearDeletePlan()
                        deletePlanDialog.close()
                    }
                }
                AppButton {
                    //% "Delete permanently"
                    text: qsTrId("aegra.repository.delete.confirm")
                    danger: true
                    enabled: !serviceClient.deletePlanBusy
                    onClicked: {
                        deletePlanDialog.close()
                        root.confirmExecuteDelete()
                    }
                }
            }
        }
    }
}
