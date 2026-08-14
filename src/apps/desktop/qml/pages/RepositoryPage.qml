import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Item {
    id: root
    property bool recoveryPointDrawerOpen: false
    property bool addPanelOpen: false
    readonly property string selectedId: serviceClient.selectedRepositoryConnectionId
    readonly property bool repositorySelected: selectedId.length > 0
    property string selectedRecoveryPointId: ""
    property string selectedRecoveryPointSummary: ""
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

    function selectRecoveryPoint(fileUuid, summaryText) {
        root.selectedRecoveryPointId = fileUuid || ""
        root.selectedRecoveryPointSummary = summaryText || ""
    }

    function requestDeletePlan() {
        if (!root.selectedRecoveryPointId || root.selectedRecoveryPointId.length === 0)
            return
        if (!serviceClient.planDeleteRecoveryPoint(root.selectedRecoveryPointId)) {
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
            root.selectedRecoveryPointId = ""
            root.selectedRecoveryPointSummary = ""
            //% "Recovery points deleted"
            serviceClient.showToast(qsTrId("aegra.repository.delete.done"), false)
            root.refreshStorageStats()
        }
        function onRepositoryChanged() {
            root.rememberRecoveryPointCount()
            root.refreshStorageStats()
        }
        function onRepositoryCommandChanged() {
            if (!serviceClient.repositoryCommandBusy) {
                if (serviceClient.repositoryRefreshRunning
                        && serviceClient.repositoryCommandErrorText.length > 0) {
                    serviceClient.showToast(serviceClient.repositoryCommandErrorText, true)
                }
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
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 120
                                    //% "NAME"
                                    text: qsTrId("aegra.repository.column.name")
                                    color: Theme.colorTextDim
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.letterSpacing: 0.8
                                    font.family: Theme.fontFamily
                                }
                                Text {
                                    Layout.preferredWidth: 90
                                    Layout.minimumWidth: 90
                                    Layout.maximumWidth: 90
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
                                    Layout.preferredWidth: 72
                                    Layout.minimumWidth: 72
                                    Layout.maximumWidth: 72
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
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 120
                                    text: displayName
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 14
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }

                                Text {
                                    Layout.preferredWidth: 90
                                    Layout.minimumWidth: 90
                                    Layout.maximumWidth: 90
                                    text: stateText
                                    color: isAvailable && !isRefreshing
                                           ? Theme.colorGreen : Theme.colorTextGrey
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
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
                                    Text {
                                        anchors.centerIn: parent
                                        visible: !isDefault
                                        text: "\u2013"
                                        color: Theme.colorTextDim
                                        font.pixelSize: 13
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

                                // Actions — fixed width, right-aligned icons
                                Item {
                                    Layout.preferredWidth: 72
                                    Layout.minimumWidth: 72
                                    Layout.maximumWidth: 72
                                    Layout.fillHeight: true

                                    Row {
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 4

                                        Rectangle {
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

                                        Rectangle {
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

                        onClicked: root.recoveryPointDrawerOpen = false
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
                                 && root.selectedRecoveryPointId.length > 0
                                 && !serviceClient.deletePlanBusy
                                 && !serviceClient.repositoryLoading
                        onClicked: root.requestDeletePlan()
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        visible: root.selectedRecoveryPointId.length > 0
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

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    color: Theme.colorTableHeader

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 10

                        Item { Layout.preferredWidth: 28 }
                        Text {
                            Layout.fillWidth: true
                            //% "Recovery point"
                            text: qsTrId("aegra.repository.column.recovery_point")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                        Text {
                            Layout.preferredWidth: 120
                            //% "Backup time"
                            text: qsTrId("aegra.repository.column.backup_time")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                        Text {
                            Layout.preferredWidth: 60
                            //% "Type"
                            text: qsTrId("aegra.repository.column.type")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                        Text {
                            Layout.preferredWidth: 80
                            //% "Logical size"
                            text: qsTrId("aegra.repository.column.logical_size")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                        Text {
                            visible: drawerPanel.width >= 800
                            Layout.preferredWidth: 80
                            //% "Stored size"
                            text: qsTrId("aegra.repository.column.stored_size")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                        Text {
                            Layout.preferredWidth: 72
                            //% "Backup chain"
                            text: qsTrId("aegra.repository.column.chain")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                    }
                }

                ListView {
                    id: recoveryPointList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: serviceClient.recoveryPoints

                    delegate: Rectangle {
                        required property string fileUuid
                        required property string backupSetUuid
                        required property string createdText
                        required property string backupTypeText
                        required property string logicalSizeText
                        required property string storedSizeText
                        required property string deduplicatedLogicalBytesText
                        required property string chainStateText
                        required property bool chainComplete
                        required property string parentSummaryText
                        required property int chainDepth
                        required property bool isBaseline
                        required property int index
                        readonly property bool selected: root.selectedRecoveryPointId === fileUuid
                        width: recoveryPointList.width
                        height: 64
                        color: selected
                               ? Qt.rgba(Theme.colorAccentBlue.r, Theme.colorAccentBlue.g,
                                         Theme.colorAccentBlue.b, 0.18)
                               : (index % 2 === 0 ? Theme.colorTableRow : Theme.colorTableAlt)

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 10

                            Rectangle {
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 16
                                Layout.leftMargin: 6
                                radius: 3
                                color: selected ? Theme.colorAccentBlue : "transparent"
                                border.width: selected ? 0 : 1
                                border.color: Theme.colorBorder
                                Text {
                                    anchors.centerIn: parent
                                    visible: selected
                                    text: "\u2713"
                                    color: "white"
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                            }

                            Column {
                                Layout.fillWidth: true
                                spacing: 2

                                Text {
                                    width: parent.width
                                    text: fileUuid
                                    color: Theme.colorTextWhite
                                    font.family: "Consolas"
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }
                                Text {
                                    width: parent.width
                                    text: parentSummaryText
                                          + (chainDepth > 1
                                             ? (" · " + qsTrId("aegra.repository.chain.depth")
                                                .arg(chainDepth))
                                             : "")
                                    color: Theme.colorTextDim
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                }
                            }

                            Text {
                                Layout.preferredWidth: 120
                                text: createdText
                                color: Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.preferredWidth: 60
                                text: backupTypeText
                                color: isBaseline ? Theme.colorTextWhite : Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.bold: isBaseline
                            }
                            Text {
                                Layout.preferredWidth: 80
                                text: logicalSizeText
                                color: Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                            }
                            Text {
                                visible: drawerPanel.width >= 800
                                Layout.preferredWidth: 80
                                text: storedSizeText
                                color: Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                            }
                            Text {
                                visible: drawerPanel.width >= 900
                                Layout.preferredWidth: 72
                                //% "Deduplicated"
                                text: qsTrId("aegra.repository.dedup.bytes") + ": "
                                      + deduplicatedLogicalBytesText
                                color: Theme.colorTextDim
                                font.family: Theme.fontFamily
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.preferredWidth: 72
                                text: chainStateText
                                color: chainComplete
                                       ? Theme.colorGreen : Theme.colorAccentRed
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.bold: true
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectRecoveryPoint(
                                           fileUuid,
                                           backupTypeText + " · " + createdText)
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0.00; color: "transparent" }
                                GradientStop { position: 0.15; color: Theme.colorBorder }
                                GradientStop { position: 0.85; color: Theme.colorBorder }
                                GradientStop { position: 1.00; color: "transparent" }
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
                //% "The service planned to delete %1 recovery point(s) in this chain. Other recovery points are kept. This cannot be undone."
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
