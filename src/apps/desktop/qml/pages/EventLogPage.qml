import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Task Log: terminal (completed) jobs from ListJobs scope=terminal.
Item {
    id: root
    //% "Task Log"
    Accessible.name: qsTrId("aegra.nav.event_log")

    property int timeIndex: 0
    property int typeIndex: 0
    property int statusIndex: 0

    readonly property var logModel: (typeof serviceClient !== "undefined" && serviceClient)
                                    ? serviceClient.taskLog : null
    readonly property bool logLoading: (typeof serviceClient !== "undefined" && serviceClient)
                                       ? serviceClient.taskLogLoading : false
    readonly property bool logHasMore: (typeof serviceClient !== "undefined" && serviceClient)
                                      ? serviceClient.taskLogHasMore : false
    readonly property string logError: (typeof serviceClient !== "undefined" && serviceClient)
                                       ? (serviceClient.taskLogErrorText || "") : ""
    readonly property int logCount: logModel ? logModel.count : 0

    readonly property int backupCount: root.logModel ? root.logModel.backupCount : 0
    readonly property int restoreCount: root.logModel ? root.logModel.restoreCount : 0
    readonly property int verifyCount: root.logModel ? root.logModel.verifyCount : 0
    readonly property int opTotal: root.logModel ? root.logModel.count : 0

    property real ringAnimProgress: 0.0

    function reload() {
        if (typeof serviceClient === "undefined" || !serviceClient || !serviceClient.jobListAvailable)
            return
        serviceClient.refreshTaskLog(root.timeIndex, root.typeIndex, root.statusIndex)
    }

    function loadMore() {
        if (typeof serviceClient === "undefined" || !serviceClient)
            return
        serviceClient.loadMoreTaskLog()
    }

    // Staggered entrance: Stage 1 stat cards, Stage 2 table card
    ParallelAnimation {
        id: pageEntranceAnim

        // Stage 1: Stat cards
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
            NumberAnimation { target: root; property: "ringAnimProgress"; from: 0.0; to: 1.0; duration: 750; easing.type: Easing.OutCubic }
        }

        // Stage 2: Table card
        SequentialAnimation {
            PauseAnimation { duration: 120 }
            ParallelAnimation {
                NumberAnimation { target: logCard; property: "opacity"; from: 0; to: 1; duration: 380; easing.type: Easing.OutCubic }
                NumberAnimation { target: logCard; property: "scale"; from: 0.95; to: 1.0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
                NumberAnimation { target: logCardTrans; property: "y"; from: 36; to: 0; duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 }
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
        logCard.opacity = 0
        logCard.scale = 0.95
        logCardTrans.y = 36
        iconBox1.scale = 0.2
        iconBox1.rotation = -25
        iconBox2.scale = 0.2
        iconBox2.rotation = -25
        root.ringAnimProgress = 0.0
        pageEntranceAnim.restart()
    }

    Component.onCompleted: {
        reload()
        restartEntranceAnimation()
    }

    onVisibleChanged: {
        if (visible) {
            restartEntranceAnimation()
            reload()
        }
    }

    onTimeIndexChanged: reload()
    onTypeIndexChanged: reload()
    onStatusIndexChanged: reload()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        // ===============================================
        // TOP STAT METRIC CARDS (3 Cards Row)
        // ===============================================
        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            // Stat 1: Succeeded Tasks
            Card {
                id: statCard1
                Layout.fillWidth: true
                implicitHeight: 92
                opacity: 0
                scale: 0.95
                transform: Translate {
                    id: statCardTrans1
                    y: 36
                }

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
                            GradientStop { position: 0.0; color: "#10B981" }
                            GradientStop { position: 1.0; color: "#059669" }
                        }
                        Text { anchors.centerIn: parent; text: "✓"; color: "#ffffff"; font.pixelSize: 22; font.bold: true }
                    }

                    ColumnLayout {
                        spacing: 2
                        Text {
                            //% "Succeeded"
                            text: qsTrId("aegra.eventlog.status.success")
                            color: Theme.colorTextGrey
                            font.pixelSize: 10
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            text: "" + (root.logModel ? root.logModel.succeededCount : 0)
                            color: Theme.colorTextWhite
                            font.pixelSize: 20
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }
                }
            }

            // Stat 2: Failed Tasks
            Card {
                id: statCard2
                Layout.fillWidth: true
                implicitHeight: 92
                opacity: 0
                scale: 0.95
                transform: Translate {
                    id: statCardTrans2
                    y: 36
                }

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
                            GradientStop { position: 0.0; color: "#EF4444" }
                            GradientStop { position: 1.0; color: "#DC2626" }
                        }
                        Text { anchors.centerIn: parent; text: "✕"; color: "#ffffff"; font.pixelSize: 20; font.bold: true }
                    }

                    ColumnLayout {
                        spacing: 2
                        Text {
                            //% "Failed"
                            text: qsTrId("aegra.task.state.failed")
                            color: Theme.colorTextGrey
                            font.pixelSize: 10
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            text: "" + (root.logModel ? root.logModel.failedCount : 0)
                            color: Theme.colorTextWhite
                            font.pixelSize: 20
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }
                }
            }

            // Stat 3: Task Types with Multi-color Ring Donut Chart
            Card {
                id: statCard3
                Layout.fillWidth: true
                implicitHeight: 92
                opacity: 0
                scale: 0.95
                transform: Translate {
                    id: statCardTrans3
                    y: 36
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 14

                    // Multi-color Ring Donut Chart
                    Item {
                        id: ringItem
                        width: 48
                        height: 48
                        Layout.alignment: Qt.AlignVCenter

                        Canvas {
                            id: ringCanvas
                            anchors.fill: parent

                            property real progress: root.ringAnimProgress
                            property int bCount: root.backupCount
                            property int rCount: root.restoreCount
                            property int vCount: root.verifyCount
                            property int tot: root.opTotal

                            onProgressChanged: requestPaint()
                            onBCountChanged: requestPaint()
                            onRCountChanged: requestPaint()
                            onVCountChanged: requestPaint()
                            onTotChanged: requestPaint()

                            onPaint: {
                                var ctx = getContext("2d")
                                var cx = width / 2
                                var cy = height / 2
                                var lw = 5.5
                                var r = (width - lw) / 2
                                ctx.reset()
                                ctx.lineCap = "round"

                                if (tot <= 0) {
                                    ctx.beginPath()
                                    ctx.arc(cx, cy, r, 0, 2 * Math.PI)
                                    ctx.strokeStyle = Theme.colorProgressTrack
                                    ctx.lineWidth = lw
                                    ctx.stroke()
                                    return
                                }

                                var segs = [
                                    { count: bCount, color: "#2A7982" },
                                    { count: rCount, color: "#3B82F6" },
                                    { count: vCount, color: "#8B5CF6" }
                                ]
                                var active = []
                                for (var i = 0; i < segs.length; ++i) {
                                    if (segs[i].count > 0) active.push(segs[i])
                                }

                                var startAngle = -Math.PI / 2
                                if (active.length === 1) {
                                    var sweep = 2 * Math.PI * progress
                                    ctx.beginPath()
                                    ctx.arc(cx, cy, r, startAngle, startAngle + sweep)
                                    ctx.strokeStyle = active[0].color
                                    ctx.lineWidth = lw
                                    ctx.stroke()
                                } else {
                                    var gap = 0.12
                                    var availableAngle = 2 * Math.PI - active.length * gap
                                    for (var j = 0; j < active.length; ++j) {
                                        var segSweep = (active[j].count / tot) * availableAngle * progress
                                        if (segSweep > 0.02) {
                                            ctx.beginPath()
                                            ctx.arc(cx, cy, r, startAngle, startAngle + segSweep)
                                            ctx.strokeStyle = active[j].color
                                            ctx.lineWidth = lw
                                            ctx.stroke()
                                        }
                                        startAngle += (active[j].count / tot) * availableAngle + gap
                                    }
                                }
                            }

                            Component.onCompleted: requestPaint()
                        }

                        // Center total text
                        Text {
                            anchors.centerIn: parent
                            text: "" + root.opTotal
                            color: Theme.colorTextWhite
                            font.pixelSize: 13
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }

                    // Legend Breakdown
                    Flow {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 12

                        // Backup
                        Row {
                            spacing: 6
                            Rectangle {
                                width: 8; height: 8; radius: 4; color: "#2A7982"
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: qsTrId("aegra.nav.backup") + " " + root.backupCount
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                        }

                        // Restore
                        Row {
                            spacing: 6
                            Rectangle {
                                width: 8; height: 8; radius: 4; color: "#3B82F6"
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: qsTrId("aegra.nav.restore") + " " + root.restoreCount
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                        }

                        // Verify
                        Row {
                            spacing: 6
                            Rectangle {
                                width: 8; height: 8; radius: 4; color: "#8B5CF6"
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: qsTrId("aegra.job.operation.verify") + " " + root.verifyCount
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                        }
                    }
                }
            }
        }

        // ===============================================
        // SCHEDULES-STYLE TABLE CARD
        // ===============================================
        Card {
            id: logCard
            Layout.fillWidth: true
            Layout.fillHeight: true

            opacity: 0
            scale: 0.95
            transform: Translate {
                id: logCardTrans
                y: 36
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 16
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                anchors.bottomMargin: 14
                spacing: 10

                // Filter & Actions Row (without Task Log header text)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14

                    Text {
                        //% "Time"
                        text: qsTrId("aegra.eventlog.filter.time")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                    }
                    ComboBox {
                        id: timeCombo
                        Layout.preferredWidth: 120
                        Layout.preferredHeight: 30
                        model: [
                            qsTrId("aegra.eventlog.range.all"),
                            qsTrId("aegra.eventlog.range.24h"),
                            qsTrId("aegra.eventlog.range.7d"),
                            qsTrId("aegra.eventlog.range.30d")
                        ]
                        currentIndex: root.timeIndex
                        onActivated: root.timeIndex = currentIndex
                        background: Rectangle {
                            color: Theme.colorInput
                            radius: 8
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                        contentItem: Text {
                            leftPadding: 10
                            rightPadding: 22
                            text: timeCombo.displayText
                            color: Theme.colorTextWhite
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                        }
                        indicator: ComboBoxIndicator { combo: timeCombo }
                        popup: Popup {
                            y: timeCombo.height + 2
                            width: timeCombo.width
                            padding: 4
                            implicitHeight: Math.min(180, contentItem.implicitHeight + 8)
                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                model: timeCombo.popup.visible ? timeCombo.delegateModel : null
                                currentIndex: timeCombo.highlightedIndex
                            }
                            background: Rectangle {
                                color: Theme.colorPopup
                                border.color: Theme.colorBorder
                                radius: 8
                            }
                        }
                        delegate: ItemDelegate {
                            width: timeCombo.width
                            height: 28
                            contentItem: Text {
                                text: modelData
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 8
                            }
                            background: Rectangle {
                                radius: 4
                                color: parent.highlighted ? Theme.colorHover : "transparent"
                            }
                        }
                    }

                    Text {
                        //% "Type"
                        text: qsTrId("aegra.home.column.type")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                    }
                    ComboBox {
                        id: typeCombo
                        Layout.preferredWidth: 100
                        Layout.preferredHeight: 30
                        model: [
                            qsTrId("aegra.eventlog.type.all"),
                            qsTrId("aegra.nav.backup"),
                            qsTrId("aegra.nav.restore"),
                            qsTrId("aegra.job.operation.verify")
                        ]
                        currentIndex: root.typeIndex
                        onActivated: root.typeIndex = currentIndex
                        background: Rectangle {
                            color: Theme.colorInput
                            radius: 8
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                        contentItem: Text {
                            leftPadding: 10
                            rightPadding: 22
                            text: typeCombo.displayText
                            color: Theme.colorTextWhite
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                        }
                        indicator: ComboBoxIndicator { combo: typeCombo }
                        popup: Popup {
                            y: typeCombo.height + 2
                            width: typeCombo.width
                            padding: 4
                            implicitHeight: Math.min(160, contentItem.implicitHeight + 8)
                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                model: typeCombo.popup.visible ? typeCombo.delegateModel : null
                                currentIndex: typeCombo.highlightedIndex
                            }
                            background: Rectangle {
                                color: Theme.colorPopup
                                border.color: Theme.colorBorder
                                radius: 8
                            }
                        }
                        delegate: ItemDelegate {
                            width: typeCombo.width
                            height: 28
                            contentItem: Text {
                                text: modelData
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 8
                            }
                            background: Rectangle {
                                radius: 4
                                color: parent.highlighted ? Theme.colorHover : "transparent"
                            }
                        }
                    }

                    Text {
                        //% "Status"
                        text: qsTrId("aegra.home.column.status")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                    }
                    ComboBox {
                        id: statusCombo
                        Layout.preferredWidth: 110
                        Layout.preferredHeight: 30
                        model: [
                            qsTrId("aegra.eventlog.status.all"),
                            qsTrId("aegra.eventlog.status.success"),
                            qsTrId("aegra.task.state.failed"),
                            qsTrId("aegra.task.state.cancelled")
                        ]
                        currentIndex: root.statusIndex
                        onActivated: root.statusIndex = currentIndex
                        background: Rectangle {
                            color: Theme.colorInput
                            radius: 8
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                        contentItem: Text {
                            leftPadding: 10
                            rightPadding: 22
                            text: statusCombo.displayText
                            color: Theme.colorTextWhite
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                        }
                        indicator: ComboBoxIndicator { combo: statusCombo }
                        popup: Popup {
                            y: statusCombo.height + 2
                            width: statusCombo.width
                            padding: 4
                            implicitHeight: Math.min(160, contentItem.implicitHeight + 8)
                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                model: statusCombo.popup.visible ? statusCombo.delegateModel : null
                                currentIndex: statusCombo.highlightedIndex
                            }
                            background: Rectangle {
                                color: Theme.colorPopup
                                border.color: Theme.colorBorder
                                radius: 8
                            }
                        }
                        delegate: ItemDelegate {
                            width: statusCombo.width
                            height: 28
                            contentItem: Text {
                                text: modelData
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 8
                            }
                            background: Rectangle {
                                radius: 4
                                color: parent.highlighted ? Theme.colorHover : "transparent"
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }

                    AppButton {
                        //% "Refresh"
                        text: qsTrId("aegra.common.refresh")
                        enabled: !root.logLoading
                        onClicked: root.reload()
                    }
                }

                // Table Header — uppercase, muted, letter-spaced (Standings style)
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 8

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
                            Layout.preferredWidth: 120
                            //% "Type"
                            text: qsTrId("aegra.home.column.type").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 160
                            Layout.fillWidth: true
                            //% "Source"
                            text: qsTrId("aegra.backup.section.source_upper")
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 160
                            Layout.fillWidth: true
                            //% "Destination"
                            text: qsTrId("aegra.backup.section.destination_upper")
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
                            text: qsTrId("aegra.home.column.status").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Text {
                            Layout.preferredWidth: 150
                            //% "Started"
                            text: qsTrId("aegra.home.column.started").toUpperCase()
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 0.8
                            font.family: Theme.fontFamily
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }

                // Table Rows
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Text {
                        anchors.centerIn: parent
                        visible: !root.logLoading && root.logCount === 0
                        text: root.logError.length > 0 ? root.logError
                                                       : qsTrId("aegra.eventlog.empty")
                        color: root.logError.length > 0 ? Theme.colorAccentRed : Theme.colorTextGrey
                        font.pixelSize: 13
                        font.family: Theme.fontFamily
                    }

                    BusyIndicator {
                        anchors.centerIn: parent
                        running: root.logLoading && root.logCount === 0
                        visible: running
                    }

                    ListView {
                        id: eventList
                        anchors.fill: parent
                        clip: true
                        spacing: 0
                        model: root.logModel
                        visible: root.logCount > 0
                        boundsBehavior: Flickable.StopAtBounds

                        delegate: Item {
                            id: logRow
                            width: eventList.width
                            height: 52

                            required property int index
                            required property string operationText
                            required property string sourceName
                            required property string destinationName
                            required property string destinationPath
                            required property string stateText
                            required property color stateColor
                            required property string createdText

                            // Subtle top divider
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                height: 1
                                color: Theme.colorBorder
                                opacity: 0.55
                            }

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
                                color: rowHover.hovered ? Theme.colorHover : "transparent"
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 8

                                // Rank #
                                Text {
                                    Layout.preferredWidth: 28
                                    text: "" + (logRow.index + 1)
                                    color: Theme.colorTextDim
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }

                                // Type with Mini-badge
                                Item {
                                    Layout.preferredWidth: 120
                                    Layout.fillHeight: true

                                    Row {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        spacing: 10

                                        Rectangle {
                                            width: 28
                                            height: 28
                                            radius: 8
                                            color: Theme.colorHover
                                            border.width: 1
                                            border.color: Theme.colorBorder

                                            Rectangle {
                                                anchors.centerIn: parent
                                                width: 18
                                                height: 18
                                                radius: 5
                                                color: {
                                                    var op = (logRow.operationText || "").toLowerCase()
                                                    if (op.indexOf("backup") >= 0) return "#2A7982"
                                                    if (op.indexOf("restore") >= 0) return "#3B82F6"
                                                    if (op.indexOf("verify") >= 0) return "#8B5CF6"
                                                    return "#10B981"
                                                }
                                                opacity: 0.92
                                            }
                                            Text {
                                                anchors.centerIn: parent
                                                text: {
                                                    var op = (logRow.operationText || "?").trim()
                                                    return op.length > 0 ? op.charAt(0).toUpperCase() : "?"
                                                }
                                                color: "#ffffff"
                                                font.pixelSize: 10
                                                font.bold: true
                                                font.family: Theme.fontFamily
                                            }
                                        }

                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: logRow.operationText || ""
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 13
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideRight
                                        }
                                    }
                                }

                                // Source
                                Text {
                                    Layout.preferredWidth: 160
                                    Layout.fillWidth: true
                                    text: logRow.sourceName || ""
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideMiddle
                                }

                                // Destination
                                Item {
                                    Layout.preferredWidth: 160
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Column {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        spacing: 2
                                        Text {
                                            width: parent.width
                                            text: logRow.destinationName || ""
                                            color: Theme.colorTextGrey
                                            font.pixelSize: 13
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                            horizontalAlignment: Text.AlignHCenter
                                            elide: Text.ElideMiddle
                                        }
                                        Text {
                                            width: parent.width
                                            text: logRow.destinationPath || ""
                                            color: Theme.colorTextDim
                                            font.pixelSize: 10
                                            font.family: Theme.fontFamily
                                            horizontalAlignment: Text.AlignHCenter
                                            elide: Text.ElideMiddle
                                            visible: (logRow.destinationPath || "").length > 0
                                        }
                                    }
                                }

                                // Status
                                Text {
                                    Layout.preferredWidth: 110
                                    text: logRow.stateText || ""
                                    color: logRow.stateColor
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                }

                                // Started
                                Text {
                                    Layout.preferredWidth: 150
                                    text: logRow.createdText || ""
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }
                        }
                    }
                }

                // Footer / Pagination
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    color: "transparent"

                    Rectangle {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 1
                        color: Theme.colorBorder
                        opacity: 0.55
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        Text {
                            //% "%1 items"
                            text: qsTrId("aegra.eventlog.page_range")
                                  .arg(root.logCount > 0 ? 1 : 0)
                                  .arg(root.logCount)
                                  .arg(root.logCount)
                                  .arg(1)
                                  .arg(1)
                            color: Theme.colorTextDim
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                        AppButton {
                            //% "Load more"
                            text: qsTrId("aegra.eventlog.next")
                            enabled: root.logHasMore && !root.logLoading
                            visible: root.logHasMore
                            onClicked: root.loadMore()
                        }
                    }
                }
            }
        }
    }
}
