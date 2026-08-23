import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// HomePage dashboard — gradient stat strip on top + 2-column content grid.
// All sections bind to live serviceClient data.
Item {
    id: root
    //% "Home"
    Accessible.name: qsTrId("aegra.nav.home")
    signal homeNavigate(int index)

    // ===============================================
    // LIVE DATA BINDINGS
    // ===============================================

    /// Earliest enabled schedule with a known next run.
    readonly property var nextSchedule: {
        var list = serviceClient.schedules
        var best = null
        for (var i = 0; i < list.length; i++) {
            var s = list[i]
            if (!s.enabled)
                continue
            var t = s.nextRunUtcMs
            if (t === undefined || t === null || t <= 0)
                continue
            if (best === null || t < best.nextRunUtcMs)
                best = s
        }
        return best
    }

    readonly property int scheduleEnabledCount: {
        var n = 0
        var list = serviceClient.schedules
        for (var i = 0; i < list.length; i++) {
            if (list[i].enabled)
                n++
        }
        return n
    }

    readonly property string dedupRatioText: {
        var logical = serviceClient.recoveryPoints.totalLogicalBytes
        var stored = serviceClient.recoveryPoints.totalStoredBytes
        if (logical <= 0 || stored <= 0)
            return "—"
        return (logical / stored).toFixed(2) + " : 1"
    }

    /// Volumes with a mount letter from the inventory tree, capped for the overview card.
    readonly property var homeVolumes: {
        var out = []
        var protectedIds = {}
        var schedules = serviceClient.schedules
        for (var i = 0; i < schedules.length; i++) {
            var ids = schedules[i].sourceIds || []
            for (var j = 0; j < ids.length; j++)
                protectedIds[ids[j]] = true
        }
        var tree = serviceClient.sources.disksTree
        for (var d = 0; d < tree.length && out.length < 4; d++) {
            var disk = tree[d]
            var volumes = disk.volumes
            for (var v = 0; v < volumes.length && out.length < 4; v++) {
                var volume = volumes[v]
                if (!volume.letter)
                    continue
                var capacity = volume.capacityBytes
                var free = volume.freeBytes
                out.push({
                    letter: volume.letter,
                    label: volume.name && volume.name.length > 0 ? volume.name : volume.letter,
                    meta: disk.mediaType + " · " + volume.size,
                    usedRatio: capacity > 0 ? (capacity - free) / capacity : 0,
                    usedText: "已用 " + serviceClient.formatBytes(Math.max(0, capacity - free)),
                    isProtected: protectedIds[volume.sourceId] === true
                })
            }
        }
        return out
    }

    function frequencyText(frequency) {
        var f = (frequency || "").toLowerCase()
        if (f === "daily")
            return "每日"
        if (f === "weekly")
            return "每周"
        if (f === "monthly")
            return "每月"
        return frequency || ""
    }

    function sourceLettersText(schedule) {
        if (!schedule)
            return "—"
        if (schedule.contentKind === 2)
            return "文件备份"
        var ids = schedule.sourceIds || []
        var letters = []
        var tree = serviceClient.sources.disksTree
        for (var d = 0; d < tree.length; d++) {
            var volumes = tree[d].volumes
            for (var v = 0; v < volumes.length; v++) {
                var volume = volumes[v]
                if (volume.letter && ids.indexOf(volume.sourceId) >= 0)
                    letters.push(volume.letter)
            }
        }
        if (letters.length === 0)
            return ids.length + " 个卷"
        return letters.join(" & ") + " 盘"
    }

    /// Free-space text plus host-volume usage ratio for a repository locator.
    /// inventoryRevision forces re-evaluation when the inventory snapshot changes.
    function repoSpaceInfo(locator, available, inventoryRevision) {
        if (!available)
            return { text: "不可用", ratio: 0, hasBar: false }
        var letter = ""
        if (locator && locator.length >= 2 && locator.charAt(1) === ":")
            letter = locator.substring(0, 2).toUpperCase()
        if (letter.length > 0) {
            var tree = serviceClient.sources.disksTree
            for (var d = 0; d < tree.length; d++) {
                var volumes = tree[d].volumes
                for (var v = 0; v < volumes.length; v++) {
                    var volume = volumes[v]
                    if (volume.letter.toUpperCase() !== letter)
                        continue
                    var capacity = volume.capacityBytes
                    var free = volume.freeBytes
                    return {
                        text: "剩余 " + serviceClient.formatBytes(free) + " / "
                              + serviceClient.formatBytes(capacity),
                        ratio: capacity > 0 ? (capacity - free) / capacity : 0,
                        hasBar: true
                    }
                }
            }
        }
        var freeBytes = serviceClient.freeBytesForLocator(locator)
        if (freeBytes >= 0)
            return { text: "剩余 " + serviceClient.formatBytes(freeBytes), ratio: 0, hasBar: false }
        return { text: "", ratio: 0, hasBar: false }
    }

    function refreshAll() {
        serviceClient.refreshRepository()
        serviceClient.refreshInventory()
        serviceClient.refreshConnections()
        serviceClient.refreshSchedules()
        serviceClient.refreshMountSessions()
        serviceClient.refreshJobs()
        // 30-day window for the task health stat card.
        serviceClient.refreshTaskLog(3, 0, 0)
    }

    Connections {
        target: serviceClient
        function onStateChanged() {
            if (serviceClient.connected && root.visible)
                root.refreshAll()
        }
    }

    // Paired row heights so left/right column cards line up.
    readonly property real contentRow1Height:
        Math.max(186, 70 + Math.max(1, homeVolumes.length) * 58)
    readonly property real contentRow2Height:
        Math.max(66 + Math.max(1, serviceClient.jobs.activeCount) * 66,
                 70 + Math.max(1, serviceClient.mountSessions.length) * 54)
    readonly property real contentRow3Height:
        Math.max(70 + Math.max(1, serviceClient.connections.count) * 58, 150)

    // ===============================================
    // Staggered Entrance Animation States
    // ===============================================
    property bool animStage1: false
    property bool animStage2: false
    property bool animStage3: false
    property bool animStage4: false

    function restartEntranceAnimation() {
        animStage1 = false
        animStage2 = false
        animStage3 = false
        animStage4 = false
        t1.restart()
    }

    Timer { id: t1; interval: 50;  repeat: false; onTriggered: root.animStage1 = true }
    Timer { id: t2; interval: 150; repeat: false; onTriggered: root.animStage2 = true }
    Timer { id: t3; interval: 260; repeat: false; onTriggered: root.animStage3 = true }
    Timer { id: t4; interval: 380; repeat: false; onTriggered: root.animStage4 = true }

    onAnimStage1Changed: if (animStage1) t2.restart()
    onAnimStage2Changed: if (animStage2) t3.restart()
    onAnimStage3Changed: if (animStage3) t4.restart()

    Component.onCompleted: {
        restartEntranceAnimation()
        refreshAll()
    }
    onVisibleChanged: {
        if (visible) {
            restartEntranceAnimation()
            refreshAll()
        }
    }

    // ===============================================
    // Reusable pieces
    // ===============================================

    // Card with the shared spring entrance (fade + rise + scale)
    component AnimCard: Card {
        property bool animOn: false
        opacity: animOn ? 1 : 0
        scale: animOn ? 1.0 : 0.95
        transform: Translate {
            y: animOn ? 0 : 36
            Behavior on y { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
        }
        Behavior on opacity { NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
    }

    // Top gradient stat card (CoachPro-style colorful tiles)
    component GradientStatCard: Rectangle {
        id: statCard
        property string label: ""
        property string value: ""
        property string emoji: ""
        property color gradStart: "#43C59E"
        property color gradEnd: "#2E9E7E"
        property string footLabel: ""
        property string footValue: ""
        property int navIndex: -1
        property bool animOn: false
        property int animIndex: 0
        signal navigate(int index)

        radius: Theme.radiusCard
        implicitHeight: 136
        gradient: Gradient {
            GradientStop { position: 0.0; color: statCard.gradStart }
            GradientStop { position: 1.0; color: statCard.gradEnd }
        }

        // Soft bottom shadow like Card
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 14
            anchors.bottomMargin: -4
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            radius: statCard.radius
            color: Theme.colorCardShadow
            opacity: 0.25
            visible: Theme.colorCardShadow.a > 0
        }

        opacity: animOn ? 1 : 0
        scale: animOn ? (statMouse.containsMouse ? 1.02 : 1.0) : 0.9
        transform: Translate {
            y: statCard.animOn ? 0 : 30
            Behavior on y { NumberAnimation { duration: 520 + statCard.animIndex * 70; easing.type: Easing.OutBack; easing.overshoot: 1.3 } }
        }
        Behavior on opacity { NumberAnimation { duration: 360 + animIndex * 70; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        MouseArea {
            id: statMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: statCard.navIndex >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: if (statCard.navIndex >= 0) statCard.navigate(statCard.navIndex)
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                ColumnLayout {
                    spacing: 3
                    Layout.fillWidth: true
                    Text { text: statCard.label; color: Qt.rgba(1, 1, 1, 0.85); font.pixelSize: 12; font.bold: true; font.family: Theme.fontFamily }
                    Text { text: statCard.value; color: "#ffffff"; font.pixelSize: 25; font.bold: true; font.family: Theme.fontFamily; elide: Text.ElideRight; Layout.fillWidth: true }
                }

                Rectangle {
                    width: 38
                    height: 38
                    radius: 12
                    color: Qt.rgba(1, 1, 1, 0.22)
                    Layout.alignment: Qt.AlignTop
                    transformOrigin: Item.Center
                    Text { anchors.centerIn: parent; text: statCard.emoji; font.pixelSize: 17 }
                    scale: statCard.animOn ? 1.0 : 0.2
                    rotation: statCard.animOn ? 0 : -25
                    Behavior on scale { NumberAnimation { duration: 680 + statCard.animIndex * 60; easing.type: Easing.OutBack; easing.overshoot: 1.8 } }
                    Behavior on rotation { NumberAnimation { duration: 680 + statCard.animIndex * 60; easing.type: Easing.OutBack; easing.overshoot: 1.8 } }
                }
            }

            Item { Layout.fillHeight: true }

            Rectangle { Layout.fillWidth: true; height: 1; color: Qt.rgba(1, 1, 1, 0.35) }

            RowLayout {
                Layout.fillWidth: true
                Text { text: statCard.footLabel; color: Qt.rgba(1, 1, 1, 0.85); font.pixelSize: 11; font.family: Theme.fontFamily }
                Item { Layout.fillWidth: true }
                Text { text: statCard.footValue; color: "#ffffff"; font.pixelSize: 13; font.bold: true; font.family: Theme.fontFamily }
            }
        }
    }

    // Small pill badge (e.g. 加密 / 重删 / 受保护)
    component PillBadge: Rectangle {
        property string text: ""
        property color fg: Theme.colorAccentBlue
        property color bg: Theme.colorHover
        implicitWidth: pillText.implicitWidth + 16
        implicitHeight: 20
        radius: 10
        color: bg
        Text {
            id: pillText
            anchors.centerIn: parent
            text: parent.text
            color: parent.fg
            font.pixelSize: 10
            font.bold: true
            font.family: Theme.fontFamily
        }
    }

    // Thin usage bar (track + fill)
    component UsageBar: Rectangle {
        property real ratio: 0
        property color fillColor: Theme.colorMenuActive
        height: 6
        radius: 3
        color: Theme.colorProgressTrack
        Rectangle {
            width: parent.width * Math.max(0, Math.min(1, parent.ratio))
            height: parent.height
            radius: parent.radius
            color: parent.fillColor
            Behavior on width { NumberAnimation { duration: 700; easing.type: Easing.OutCubic } }
        }
    }

    // Muted placeholder row for empty lists
    component EmptyHint: Text {
        color: Theme.colorTextDim
        font.pixelSize: 12
        font.family: Theme.fontFamily
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: 14
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: mainCol.implicitHeight + 40
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: mainCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 24
            spacing: 20

            // ===============================================
            // TOP STAT STRIP — 4 gradient cards
            // ===============================================
            RowLayout {
                Layout.fillWidth: true
                spacing: 18

                GradientStatCard {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 100
                    animOn: root.animStage1
                    animIndex: 0
                    label: "已保护数据量"
                    value: serviceClient.formatBytes(serviceClient.recoveryPoints.totalLogicalBytes)
                    emoji: "🛡️"
                    gradStart: Theme.colorMenuActive
                    gradEnd: Theme.colorMenuActiveEnd
                    footLabel: "恢复点总数"
                    footValue: String(serviceClient.recoveryPointCount)
                }

                GradientStatCard {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 100
                    animOn: root.animStage1
                    animIndex: 1
                    label: "存储总占用"
                    value: serviceClient.formatBytes(serviceClient.recoveryPoints.totalStoredBytes)
                    emoji: "🗄️"
                    gradStart: Theme.colorAccentBlue
                    gradEnd: Qt.darker(Theme.colorAccentBlue, 1.25)
                    footLabel: "重删与压缩率"
                    footValue: root.dedupRatioText
                }

                GradientStatCard {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 100
                    animOn: root.animStage1
                    animIndex: 2
                    label: "近 30 天任务"
                    value: String(serviceClient.taskLog.count + serviceClient.jobs.activeCount)
                    emoji: "📊"
                    gradStart: Theme.colorGreen
                    gradEnd: Qt.darker(Theme.colorGreen, 1.3)
                    footLabel: "成功 / 运行中 / 失败"
                    footValue: serviceClient.taskLog.succeededCount + " / "
                               + serviceClient.jobs.activeCount + " / "
                               + serviceClient.taskLog.failedCount
                    navIndex: 5
                    onNavigate: function(navTo) { root.homeNavigate(navTo) }
                }

                GradientStatCard {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 100
                    animOn: root.animStage1
                    animIndex: 3
                    label: "备份计划"
                    value: String(serviceClient.schedules.length)
                    emoji: "📅"
                    gradStart: Theme.colorAccentRed
                    gradEnd: Qt.darker(Theme.colorAccentRed, 1.25)
                    footLabel: "已启用 " + root.scheduleEnabledCount + " · 下次备份"
                    footValue: root.nextSchedule ? root.nextSchedule.nextRun : "—"
                    navIndex: 1
                    onNavigate: function(navTo) { root.homeNavigate(navTo) }
                }
            }

            // ===============================================
            // MAIN 2-COLUMN CONTENT GRID
            // ===============================================
            RowLayout {
                Layout.fillWidth: true
                spacing: 20
                Layout.alignment: Qt.AlignTop

                // ===============================================
                // LEFT COLUMN
                // ===============================================
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 560
                    spacing: 20
                    Layout.alignment: Qt.AlignTop

                    // Card ①: 下一次计划备份
                    AnimCard {
                        Layout.fillWidth: true
                        implicitHeight: root.contentRow1Height
                        title: "下一次计划备份"
                        actionText: "管理备份计划"
                        onActionClicked: root.homeNavigate(1)
                        animOn: root.animStage2

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.topMargin: 52
                            anchors.bottomMargin: 18
                            anchors.leftMargin: 22
                            anchors.rightMargin: 22
                            spacing: 14
                            visible: root.nextSchedule !== null

                            Item { Layout.fillHeight: true }

                            // Schedule name + time + option badges
                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 10
                                PillBadge {
                                    text: "🛡️ " + (root.nextSchedule ? root.nextSchedule.displayName : "")
                                          + " · " + (root.nextSchedule && root.nextSchedule.backupType === 1 ? "完整" : "增量")
                                    implicitHeight: 24
                                    radius: 12
                                }
                                Text {
                                    text: root.nextSchedule
                                          ? root.nextSchedule.nextRun + " · " + root.frequencyText(root.nextSchedule.frequency)
                                          : ""
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                PillBadge { visible: root.nextSchedule !== null && root.nextSchedule.encryptionEnabled === true; text: "🔒 加密"; fg: Theme.colorGreen; bg: Theme.colorToastSuccessBg }
                                PillBadge { visible: root.nextSchedule !== null && root.nextSchedule.deduplicationEnabled === true; text: "♻️ 重删"; fg: Theme.colorGreen; bg: Theme.colorToastSuccessBg }
                            }

                            // Source -> SYNC -> Target
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 16

                                Item { Layout.fillWidth: true }

                                Rectangle {
                                    width: 170
                                    height: 52
                                    radius: 14
                                    color: Theme.colorInput
                                    border.width: 1
                                    border.color: Theme.colorBorder
                                    Row {
                                        anchors.centerIn: parent
                                        spacing: 10
                                        Text { text: "💾"; font.pixelSize: 20; anchors.verticalCenter: parent.verticalCenter }
                                        Text {
                                            text: root.sourceLettersText(root.nextSchedule)
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 14
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                            anchors.verticalCenter: parent.verticalCenter
                                        }
                                    }
                                }

                                Rectangle {
                                    width: 38
                                    height: 38
                                    radius: 19
                                    scale: root.animStage2 ? 1.0 : 0.3
                                    Behavior on scale { NumberAnimation { duration: 500; easing.type: Easing.OutBack; easing.overshoot: 1.5 } }
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "#FF7B8B" }
                                        GradientStop { position: 1.0; color: "#EE6476" }
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        text: "SYNC"
                                        color: "#ffffff"
                                        font.pixelSize: 10
                                        font.bold: true
                                        font.family: Theme.fontFamily
                                    }
                                }

                                Rectangle {
                                    width: 170
                                    height: 52
                                    radius: 14
                                    color: Theme.colorInput
                                    border.width: 1
                                    border.color: Theme.colorBorder
                                    Row {
                                        anchors.centerIn: parent
                                        spacing: 10
                                        Text { text: "🗄️"; font.pixelSize: 20; anchors.verticalCenter: parent.verticalCenter }
                                        Text {
                                            text: root.nextSchedule && root.nextSchedule.destinationName
                                                  ? root.nextSchedule.destinationName : "默认仓库"
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 14
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                            anchors.verticalCenter: parent.verticalCenter
                                        }
                                    }
                                }

                                Item { Layout.fillWidth: true }
                            }

                            Item { Layout.fillHeight: true }
                        }

                        // Empty state: no enabled schedule with a next run
                        ColumnLayout {
                            anchors.centerIn: parent
                            anchors.verticalCenterOffset: 20
                            spacing: 12
                            visible: root.nextSchedule === null

                            Text {
                                text: "尚未配置启用的备份计划"
                                color: Theme.colorTextDim
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                Layout.alignment: Qt.AlignHCenter
                            }
                            Rectangle {
                                width: 132
                                height: 32
                                radius: 16
                                color: emptyPlanMouse.containsMouse ? Theme.colorButtonHover : Theme.colorButton
                                Layout.alignment: Qt.AlignHCenter
                                Behavior on color { ColorAnimation { duration: 150 } }
                                Text {
                                    anchors.centerIn: parent
                                    text: "新建备份计划"
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 12
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                }
                                MouseArea {
                                    id: emptyPlanMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.homeNavigate(1)
                                }
                            }
                        }
                    }

                    // Card ②: 正在运行的任务
                    AnimCard {
                        Layout.fillWidth: true
                        implicitHeight: root.contentRow2Height
                        title: "正在运行的任务"
                        actionText: "查看任务日志"
                        onActionClicked: root.homeNavigate(5)
                        animOn: root.animStage3

                        ColumnLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 50
                            anchors.leftMargin: 20
                            anchors.rightMargin: 20
                            spacing: 10

                            EmptyHint {
                                visible: serviceClient.jobs.activeCount === 0
                                text: "当前没有正在运行的任务"
                            }

                            Repeater {
                                model: serviceClient.jobs
                                delegate: ColumnLayout {
                                    required property int index
                                    required property bool isActive
                                    required property string operationText
                                    required property string sourceName
                                    required property string messageText
                                    required property string stateText
                                    required property int progressPercent
                                    required property bool progressVisible

                                    visible: isActive
                                    Layout.fillWidth: true
                                    spacing: 6

                                    opacity: root.animStage3 ? 1 : 0
                                    Behavior on opacity { NumberAnimation { duration: 320 + index * 60; easing.type: Easing.OutCubic } }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 8
                                        Layout.rightMargin: 8
                                        spacing: 12
                                        Rectangle {
                                            width: 32
                                            height: 32
                                            radius: 10
                                            color: Theme.colorHover
                                            Text {
                                                anchors.centerIn: parent
                                                text: operationText.indexOf("备份") >= 0 || operationText.toLowerCase().indexOf("backup") >= 0 ? "💾" : "🔍"
                                                font.pixelSize: 15
                                            }
                                        }
                                        ColumnLayout {
                                            spacing: 0
                                            Layout.fillWidth: true
                                            Text {
                                                text: operationText + (sourceName.length > 0 ? " · " + sourceName : "")
                                                color: Theme.colorTextWhite
                                                font.pixelSize: 13
                                                font.bold: true
                                                font.family: Theme.fontFamily
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Text {
                                                text: messageText.length > 0 ? messageText : stateText
                                                color: Theme.colorTextGrey
                                                font.pixelSize: 11
                                                font.family: Theme.fontFamily
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                        }
                                        Text {
                                            visible: progressVisible
                                            text: progressPercent + "%"
                                            color: Theme.colorAccentBlue
                                            font.pixelSize: 15
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                        }
                                        PillBadge { visible: !progressVisible; text: stateText; fg: Theme.colorTextGrey; bg: Theme.colorHover }
                                    }

                                    UsageBar {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 8
                                        Layout.rightMargin: 8
                                        visible: progressVisible
                                        ratio: root.animStage3 ? progressPercent / 100 : 0
                                        fillColor: Theme.colorMenuActive
                                    }
                                }
                            }
                        }
                    }

                    // Card ④: 仓库连接状态
                    AnimCard {
                        Layout.fillWidth: true
                        implicitHeight: root.contentRow3Height
                        title: "仓库连接状态"
                        actionText: "管理仓库"
                        onActionClicked: root.homeNavigate(4)
                        animOn: root.animStage4

                        ColumnLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 52
                            anchors.leftMargin: 20
                            anchors.rightMargin: 20
                            spacing: 4

                            EmptyHint {
                                visible: serviceClient.connections.count === 0
                                text: "尚未添加仓库连接"
                            }

                            Repeater {
                                model: serviceClient.connections
                                delegate: Rectangle {
                                    id: repoRow
                                    required property int index
                                    required property string displayName
                                    required property string locator
                                    required property bool isDefault
                                    required property bool isAvailable

                                    readonly property var spaceInfo:
                                        root.repoSpaceInfo(locator, isAvailable, serviceClient.sources.count)

                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 54
                                    radius: 10
                                    color: repoRowMouse.containsMouse ? Theme.colorHover : "transparent"
                                    Behavior on color { ColorAnimation { duration: 150 } }

                                    opacity: root.animStage4 ? 1 : 0
                                    transform: Translate {
                                        x: root.animStage4 ? 0 : -16
                                        Behavior on x { NumberAnimation { duration: 400 + repoRow.index * 45; easing.type: Easing.OutBack; easing.overshoot: 1.2 } }
                                    }
                                    Behavior on opacity { NumberAnimation { duration: 320 + index * 45; easing.type: Easing.OutCubic } }

                                    MouseArea {
                                        id: repoRowMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 8
                                        anchors.rightMargin: 8
                                        spacing: 12

                                        Item {
                                            width: 32
                                            height: 32
                                            Rectangle {
                                                anchors.centerIn: parent
                                                width: 10
                                                height: 10
                                                radius: 5
                                                color: repoRow.isAvailable ? Theme.colorGreen : Theme.colorAccentRed
                                            }
                                        }

                                        ColumnLayout {
                                            spacing: 0
                                            Layout.fillWidth: true
                                            RowLayout {
                                                spacing: 6
                                                Text { text: repoRow.displayName; color: Theme.colorTextWhite; font.pixelSize: 13; font.bold: true; font.family: Theme.fontFamily }
                                                PillBadge { visible: repoRow.isDefault; text: "默认"; fg: Theme.colorAccentBlue; bg: Theme.colorHover }
                                            }
                                            Text {
                                                text: repoRow.locator
                                                color: Theme.colorTextDim
                                                font.pixelSize: 11
                                                font.family: Theme.fontFamily
                                                elide: Text.ElideMiddle
                                                Layout.fillWidth: true
                                            }
                                        }

                                        ColumnLayout {
                                            spacing: 5
                                            Layout.preferredWidth: 170
                                            Text {
                                                text: repoRow.spaceInfo.text
                                                color: repoRow.isAvailable ? Theme.colorTextGrey : Theme.colorAccentRed
                                                font.pixelSize: 11
                                                font.family: Theme.fontFamily
                                                Layout.alignment: Qt.AlignRight
                                            }
                                            UsageBar {
                                                Layout.fillWidth: true
                                                visible: repoRow.spaceInfo.hasBar
                                                ratio: root.animStage4 ? repoRow.spaceInfo.ratio : 0
                                                fillColor: repoRow.spaceInfo.ratio > 0.85 ? Theme.colorAccentRed : Theme.colorMenuActive
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ===============================================
                // RIGHT COLUMN
                // ===============================================
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 420
                    spacing: 20
                    Layout.alignment: Qt.AlignTop

                    // Card ⑥: 本机磁盘概览
                    AnimCard {
                        Layout.fillWidth: true
                        implicitHeight: root.contentRow1Height
                        title: "本机磁盘概览"
                        actionText: "新建备份计划"
                        onActionClicked: root.homeNavigate(1)
                        animOn: root.animStage2

                        ColumnLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 52
                            anchors.leftMargin: 20
                            anchors.rightMargin: 20
                            spacing: 4

                            EmptyHint {
                                visible: root.homeVolumes.length === 0
                                text: "正在加载磁盘信息…"
                            }

                            Repeater {
                                model: root.homeVolumes
                                delegate: Rectangle {
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 54
                                    radius: 10
                                    color: volRowMouse.containsMouse ? Theme.colorHover : "transparent"
                                    Behavior on color { ColorAnimation { duration: 150 } }

                                    opacity: root.animStage2 ? 1 : 0
                                    transform: Translate {
                                        x: root.animStage2 ? 0 : -16
                                        Behavior on x { NumberAnimation { duration: 400 + index * 45; easing.type: Easing.OutBack; easing.overshoot: 1.2 } }
                                    }
                                    Behavior on opacity { NumberAnimation { duration: 320 + index * 45; easing.type: Easing.OutCubic } }

                                    MouseArea {
                                        id: volRowMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 8
                                        anchors.rightMargin: 8
                                        spacing: 12

                                        Rectangle {
                                            width: 32
                                            height: 32
                                            radius: 10
                                            color: Theme.volumeColor(index)
                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.letter
                                                color: Theme.colorVolumeText
                                                font.pixelSize: 12
                                                font.bold: true
                                                font.family: Theme.fontFamily
                                            }
                                        }

                                        ColumnLayout {
                                            spacing: 0
                                            Layout.preferredWidth: 110
                                            Text { text: modelData.label; color: Theme.colorTextWhite; font.pixelSize: 13; font.bold: true; font.family: Theme.fontFamily; elide: Text.ElideRight; Layout.maximumWidth: 110 }
                                            Text { text: modelData.meta; color: Theme.colorTextDim; font.pixelSize: 10; font.family: Theme.fontFamily }
                                        }

                                        ColumnLayout {
                                            spacing: 5
                                            Layout.fillWidth: true
                                            Text {
                                                text: modelData.usedText
                                                color: Theme.colorTextGrey
                                                font.pixelSize: 10
                                                font.family: Theme.fontFamily
                                            }
                                            UsageBar {
                                                Layout.fillWidth: true
                                                ratio: root.animStage2 ? modelData.usedRatio : 0
                                                fillColor: Theme.volumeColor(index)
                                            }
                                        }

                                        PillBadge {
                                            text: modelData.isProtected ? "✓ 受保护" : "! 未保护"
                                            fg: modelData.isProtected ? Theme.colorToastSuccessBorder : Theme.colorToastErrorBorder
                                            bg: modelData.isProtected ? Theme.colorToastSuccessBg : Theme.colorToastErrorBg
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Card ⑦: 活动挂载
                    AnimCard {
                        Layout.fillWidth: true
                        implicitHeight: root.contentRow2Height
                        title: "活动挂载"
                        actionText: "挂载管理"
                        onActionClicked: root.homeNavigate(3)
                        animOn: root.animStage3

                        ColumnLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 52
                            anchors.leftMargin: 20
                            anchors.rightMargin: 20
                            spacing: 4

                            EmptyHint {
                                visible: serviceClient.mountSessions.length === 0
                                text: "暂无挂载的恢复点"
                            }

                            Repeater {
                                model: serviceClient.mountSessions
                                delegate: Rectangle {
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 50
                                    radius: 10
                                    color: mountRowMouse.containsMouse ? Theme.colorHover : "transparent"
                                    Behavior on color { ColorAnimation { duration: 150 } }

                                    opacity: root.animStage3 ? 1 : 0
                                    Behavior on opacity { NumberAnimation { duration: 320 + index * 60; easing.type: Easing.OutCubic } }

                                    MouseArea {
                                        id: mountRowMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 8
                                        anchors.rightMargin: 8
                                        spacing: 12

                                        Rectangle {
                                            width: 32
                                            height: 32
                                            radius: 10
                                            color: Theme.colorHover
                                            border.width: 1
                                            border.color: Theme.colorBorder
                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.mountPoint.substring(0, 2)
                                                color: Theme.colorAccentBlue
                                                font.pixelSize: 12
                                                font.bold: true
                                                font.family: Theme.fontFamily
                                            }
                                        }

                                        ColumnLayout {
                                            spacing: 0
                                            Layout.fillWidth: true
                                            Text {
                                                text: modelData.diskName + " · " + modelData.mountPoint
                                                color: Theme.colorTextWhite
                                                font.pixelSize: 12
                                                font.bold: true
                                                font.family: Theme.fontFamily
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Text { text: modelData.sizeText; color: Theme.colorTextDim; font.pixelSize: 10; font.family: Theme.fontFamily }
                                        }

                                        PillBadge {
                                            text: modelData.stateText
                                            fg: modelData.state === 2 ? Theme.colorToastSuccessBorder
                                                : (modelData.state === 4 ? Theme.colorToastErrorBorder : Theme.colorTextGrey)
                                            bg: modelData.state === 2 ? Theme.colorToastSuccessBg
                                                : (modelData.state === 4 ? Theme.colorToastErrorBg : Theme.colorHover)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Banner: 数据安全提醒 (CTA, pairs with 仓库连接状态 row)
                    AnimCard {
                        Layout.fillWidth: true
                        implicitHeight: root.contentRow3Height
                        animOn: root.animStage4
                        border.width: 0
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: Theme.colorMenuActive }
                            GradientStop { position: 1.0; color: Theme.colorMenuActiveEnd }
                        }

                        Text {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.rightMargin: 20
                            anchors.bottomMargin: 12
                            text: "🛰️"
                            font.pixelSize: 52
                            opacity: 0.5
                        }

                        ColumnLayout {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 22
                            anchors.rightMargin: 22
                            spacing: 10

                            Text {
                                text: "数据安全提醒"
                                color: Qt.rgba(1, 1, 1, 0.75)
                                font.pixelSize: 10
                                font.bold: true
                                font.letterSpacing: 0.8
                                font.family: Theme.fontFamily
                            }
                            Text {
                                text: "立即为重要卷配置\n异地冷备计划"
                                color: "#ffffff"
                                font.pixelSize: 18
                                font.bold: true
                                lineHeight: 1.2
                                font.family: Theme.fontFamily
                            }
                            Rectangle {
                                width: 136
                                height: 32
                                radius: 16
                                color: bannerBtnMouse.containsMouse ? Theme.colorHover : Theme.colorCard
                                border.width: 1
                                border.color: Theme.colorBorder
                                Behavior on color { ColorAnimation { duration: 150 } }

                                Text {
                                    anchors.centerIn: parent
                                    text: "前往新建备份计划"
                                    color: Theme.themeId === "dark" ? Theme.colorTextWhite : Theme.colorMenuActiveEnd
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                }
                                MouseArea {
                                    id: bannerBtnMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.homeNavigate(1)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
