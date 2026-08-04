import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Visual baseline: backup/src/gui BackupPage — schedule list + Add Schedule Wizard step 1.
// Demo data fills UI when Service inventory/locations are empty.
Item {
    id: root
    //% "Backup"
    Accessible.name: qsTrId("aegra.nav.backup")

    property bool wizardOpen: false
    property int wizardStep: 1
    property int selectedDiskIndex: -1
    property int selectedLocationIndex: 0
    property var expandedDisks: ({})

    // Local UI schedule list (wizard Create appends; Service schedule API later).
    property var schedules: [
        {
            id: 1,
            sourceName: "disk0",
            destinationName: "fff",
            destinationPath: "E:\\qqqq",
            frequency: "daily",
            timeOfDay: "02:00",
            lastRun: "",
            nextRun: "2026-08-05 02:00",
            enabled: true
        }
    ]

    // Demo disks matching old wizard look (used when Service has no inventory).
    readonly property var demoDisks: [
        {
            name: "Disk 0",
            size: "20.0 GB",
            type: "GPT",
            isSystem: false,
            volumes: [
                { name: "System Reserved", letter: "", size: "100 MB" },
                { name: "Windows", letter: "C:", size: "19.9 GB" }
            ]
        },
        {
            name: "Disk 1",
            size: "30.0 GB",
            type: "GPT",
            isSystem: true,
            volumes: [
                { name: "Data", letter: "D:", size: "30.0 GB" }
            ]
        }
    ]

    readonly property var demoLocations: [
        { name: "e", path: "E:\\", isDefault: true, type: "local" },
        { name: "444", path: "C:\\", isDefault: false, type: "local" }
    ]

    readonly property var sourceModel: {
        if (serviceClient.sources && serviceClient.sources.count > 0)
            return null // use live model via ListView below
        return demoDisks
    }

    readonly property var locationModel: {
        if (serviceClient.connections && serviceClient.connections.count > 0)
            return null
        return demoLocations
    }

    function isDiskExpanded(index) {
        return expandedDisks[index] === true
    }

    function toggleDiskExpanded(index) {
        var next = Object.assign({}, expandedDisks)
        next[index] = !next[index]
        expandedDisks = next
    }

    function openWizard() {
        selectedDiskIndex = -1
        selectedLocationIndex = 0
        expandedDisks = ({})
        wizardStep = 1
        if (wizardStep2)
            wizardStep2.resetDefaults()
        wizardOpen = true
        if (serviceClient.connected) {
            serviceClient.refreshInventory()
            serviceClient.refreshConnections()
        }
    }

    function closeWizard() {
        wizardOpen = false
        wizardStep = 1
    }

    function canGoNext() {
        return selectedDiskIndex >= 0 && selectedLocationIndex >= 0
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

    function createScheduleFromWizard() {
        var srcName = "disk" + Math.max(0, selectedDiskIndex)
        if (serviceClient.sources.count > 0 && selectedDiskIndex >= 0
                && selectedDiskIndex < serviceClient.sources.count) {
            // keep diskN style for list
        } else if (selectedDiskIndex >= 0 && selectedDiskIndex < demoDisks.length) {
            srcName = "disk" + selectedDiskIndex
        }
        var destName = "local"
        var destPath = ""
        var locs = serviceClient.connections.count > 0
                   ? null : demoLocations
        if (locs && selectedLocationIndex >= 0 && selectedLocationIndex < locs.length) {
            destName = locs[selectedLocationIndex].name
            destPath = locs[selectedLocationIndex].path
        }
        var s2 = wizardStep2
        var nextId = 1
        for (var i = 0; i < schedules.length; ++i) {
            if (schedules[i].id >= nextId)
                nextId = schedules[i].id + 1
        }
        var row = {
            id: nextId,
            sourceName: srcName,
            destinationName: destName,
            destinationPath: destPath,
            frequency: s2 ? s2.frequency : "daily",
            timeOfDay: s2 ? s2.timeOfDay : "02:00",
            lastRun: "",
            nextRun: "",
            enabled: true
        }
        var next = schedules.slice()
        next.push(row)
        schedules = next
        closeWizard()
    }

    function toggleScheduleEnabled(id) {
        var next = []
        for (var i = 0; i < schedules.length; ++i) {
            var r = Object.assign({}, schedules[i])
            if (r.id === id)
                r.enabled = !r.enabled
            next.push(r)
        }
        schedules = next
    }

    function deleteSchedule(id) {
        var next = []
        for (var i = 0; i < schedules.length; ++i) {
            if (schedules[i].id !== id)
                next.push(schedules[i])
        }
        schedules = next
    }

    // ==================== LIST ====================
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Row {
                spacing: 8
                Rectangle {
                    width: 3
                    height: 20
                    color: Theme.colorAccentBlue
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    //% "Backup"
                    text: qsTrId("aegra.nav.backup")
                    color: Theme.colorTextWhite
                    font.pixelSize: 18
                    font.bold: true
                    font.family: Theme.fontFamily
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Item { Layout.fillWidth: true }
        }

        Card {
            Layout.fillWidth: true
            Layout.fillHeight: true
            //% "Schedules"
            title: qsTrId("aegra.backup.section.schedule")

            AppButton {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.top: parent.top
                anchors.topMargin: 8
                z: 2
                //% "Add"
                text: qsTrId("aegra.common.add")
                onClicked: root.openWizard()
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 44
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                anchors.bottomMargin: 12
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    color: Theme.colorTableHeader
                    radius: 2
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 8
                        Text {
                            Layout.preferredWidth: 140
                            Layout.fillWidth: true
                            //% "Source"
                            text: qsTrId("aegra.backup.section.source")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 100
                            Layout.fillWidth: true
                            //% "Destination"
                            text: qsTrId("aegra.backup.column.destination")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 110
                            //% "Frequency"
                            text: qsTrId("aegra.backup.column.frequency")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 130
                            //% "Last run"
                            text: qsTrId("aegra.backup.column.last_run")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 130
                            //% "Next run"
                            text: qsTrId("aegra.backup.column.next_run")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 72
                            //% "Enabled"
                            text: qsTrId("aegra.backup.column.enabled")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Item { Layout.preferredWidth: 140 }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Text {
                        anchors.centerIn: parent
                        visible: root.schedules.length === 0
                        //% "No schedules"
                        text: qsTrId("aegra.backup.schedules.empty")
                        color: Theme.colorTextGrey
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

                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            width: scheduleList.width
                            height: (modelData.destinationPath || "") !== "" ? 52 : 44
                            color: index % 2 === 0 ? Theme.colorTableRow : Theme.colorTableAlt

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 8

                                Text {
                                    Layout.preferredWidth: 140
                                    Layout.fillWidth: true
                                    text: modelData.sourceName || ""
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideMiddle
                                }
                                Item {
                                    Layout.preferredWidth: 120
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Column {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        spacing: 0
                                        Text {
                                            width: parent.width
                                            height: (modelData.destinationPath || "") !== ""
                                                    ? 16 : implicitHeight
                                            text: modelData.destinationName || ""
                                            color: Theme.colorTextWhite
                                            font.pixelSize: (modelData.destinationPath || "") !== ""
                                                            ? 11 : 12
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideMiddle
                                        }
                                        Text {
                                            width: parent.width
                                            height: 14
                                            visible: (modelData.destinationPath || "") !== ""
                                            text: modelData.destinationPath || ""
                                            color: Theme.colorTextGrey
                                            font.pixelSize: 9
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideMiddle
                                        }
                                    }
                                }
                                Text {
                                    Layout.preferredWidth: 110
                                    text: root.freqLabel(modelData.frequency)
                                          + " · " + (modelData.timeOfDay || "02:00")
                                    color: Theme.colorAccentBlue
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.preferredWidth: 130
                                    text: root.timeOrNa(modelData.lastRun)
                                    color: (modelData.lastRun && ("" + modelData.lastRun).trim().length > 0)
                                           ? Theme.colorTextWhite : Theme.colorTextGrey
                                    font.pixelSize: 11
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.preferredWidth: 130
                                    text: root.timeOrNa(modelData.nextRun)
                                    color: (modelData.nextRun && ("" + modelData.nextRun).trim().length > 0)
                                           ? Theme.colorTextWhite : Theme.colorTextGrey
                                    font.pixelSize: 11
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                }
                                Item {
                                    Layout.preferredWidth: 72
                                    Layout.fillHeight: true
                                    Rectangle {
                                        width: 40
                                        height: 22
                                        radius: 11
                                        anchors.centerIn: parent
                                        color: modelData.enabled ? Theme.colorAccentBlue : "#555"
                                        border.width: 1
                                        border.color: modelData.enabled
                                                      ? Theme.colorAccentBlue : Theme.colorBorder
                                        Rectangle {
                                            width: 16
                                            height: 16
                                            radius: 8
                                            anchors.verticalCenter: parent.verticalCenter
                                            x: modelData.enabled ? parent.width - width - 3 : 3
                                            color: "white"
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.toggleScheduleEnabled(modelData.id)
                                        }
                                    }
                                }
                                Row {
                                    Layout.preferredWidth: 140
                                    spacing: 8
                                    layoutDirection: Qt.RightToLeft
                                    Rectangle {
                                        width: 60
                                        height: 28
                                        radius: 4
                                        color: delHover.containsMouse ? "#cc3333" : Theme.colorButton
                                        border.width: 1
                                        border.color: Theme.colorBorder
                                        anchors.verticalCenter: parent.verticalCenter
                                        Text {
                                            anchors.centerIn: parent
                                            //% "Delete"
                                            text: qsTrId("aegra.common.delete")
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 12
                                            font.family: Theme.fontFamily
                                        }
                                        MouseArea {
                                            id: delHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.deleteSchedule(modelData.id)
                                        }
                                    }
                                    Rectangle {
                                        width: 60
                                        height: 28
                                        radius: 4
                                        color: runHover.containsMouse
                                               ? Theme.colorButtonHover : Theme.colorButton
                                        border.width: 1
                                        border.color: Theme.colorBorder
                                        anchors.verticalCenter: parent.verticalCenter
                                        opacity: modelData.enabled ? 1.0 : 0.55
                                        Text {
                                            anchors.centerIn: parent
                                            //% "Run"
                                            text: qsTrId("aegra.backup.action.run")
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 12
                                            font.family: Theme.fontFamily
                                        }
                                        MouseArea {
                                            id: runHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: { /* UI only until Service schedule run */ }
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

    // ==================== WIZARD (old Add Schedule Wizard step 1) ====================
    Item {
        id: wizardDrawer
        anchors.fill: parent
        z: 2000

        Rectangle {
            anchors.fill: parent
            color: Theme.colorScrim
            opacity: root.wizardOpen ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 250 } }
            MouseArea {
                anchors.fill: parent
                enabled: root.wizardOpen
            }
        }

        Rectangle {
            id: wizardPanel
            width: Math.max(420, parent.width * 9 / 10)
            height: parent.height
            property real slideProgress: root.wizardOpen ? 0 : 1
            x: parent.width - width + slideProgress * width
            color: Theme.colorBg
            border.width: 1
            border.color: Theme.colorBorder
            Behavior on slideProgress {
                NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                // Header — "Add Schedule Wizard"
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    spacing: 8
                    Rectangle {
                        width: 3
                        height: 18
                        color: Theme.colorAccentBlue
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        //% "Add Schedule Wizard"
                        text: qsTrId("aegra.backup.wizard.title")
                        color: Theme.colorTextWhite
                        font.pixelSize: 16
                        font.bold: true
                        font.family: Theme.fontFamily
                        Layout.fillWidth: true
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
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: root.closeWizard()
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: root.wizardStep === 1 ? 0 : 1

                    // -------- Step 1: SOURCE | DESTINATION --------
                    ColumnLayout {
                        spacing: 12

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 20

                            // -------- SOURCE --------
                            Card {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                //% "SOURCE"
                                title: qsTrId("aegra.backup.section.source_upper")

                                ScrollView {
                                    id: sourceScroll
                                    anchors.fill: parent
                                    anchors.topMargin: 50
                                    anchors.margins: 16
                                    clip: true
                                    contentWidth: availableWidth

                                    Column {
                                        id: disksColumn
                                        width: sourceScroll.availableWidth
                                        spacing: 10

                                        // Live Service sources when available
                                        Repeater {
                                            model: serviceClient.sources
                                            visible: serviceClient.sources.count > 0
                                            delegate: sourceDiskDelegate
                                        }

                                        // Demo disks when Service empty
                                        Repeater {
                                            model: serviceClient.sources.count > 0
                                                   ? [] : root.demoDisks
                                            delegate: Rectangle {
                                                required property int index
                                                required property var modelData
                                                width: disksColumn.width
                                                height: 45
                                                radius: 4
                                                color: diskHover.containsMouse
                                                       ? Theme.colorHover : Theme.colorListItem

                                                readonly property bool selected:
                                                    root.selectedDiskIndex === index

                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 10
                                                    anchors.rightMargin: 10
                                                    spacing: 10

                                                    Rectangle {
                                                        width: 18
                                                        height: 18
                                                        radius: 3
                                                        color: selected ? Theme.colorAccentBlue
                                                                        : "transparent"
                                                        border.width: 2
                                                        border.color: selected
                                                                      ? Theme.colorAccentBlue
                                                                      : Theme.colorTextGrey
                                                        Text {
                                                            anchors.centerIn: parent
                                                            text: selected ? "\u2713" : ""
                                                            color: "white"
                                                            font.pixelSize: 12
                                                            font.bold: true
                                                        }
                                                    }

                                                    Text {
                                                        text: "\u25B6"
                                                        color: Theme.colorTextGrey
                                                        font.pixelSize: 10
                                                        Layout.preferredWidth: 15
                                                        rotation: root.isDiskExpanded(index)
                                                                  ? 90 : 0
                                                        Behavior on rotation {
                                                            NumberAnimation { duration: 150 }
                                                        }
                                                        MouseArea {
                                                            anchors.fill: parent
                                                            cursorShape: Qt.PointingHandCursor
                                                            onClicked: root.toggleDiskExpanded(index)
                                                        }
                                                    }

                                                    DiskIcon {
                                                        size: 28
                                                        variant: modelData.isSystem
                                                                 ? "system" : "hdd"
                                                    }

                                                    ColumnLayout {
                                                        Layout.fillWidth: true
                                                        spacing: 2
                                                        Text {
                                                            text: modelData.name
                                                                  + " (" + modelData.size + ")"
                                                            color: Theme.colorTextWhite
                                                            font.pixelSize: 13
                                                            font.bold: true
                                                            font.family: Theme.fontFamily
                                                        }
                                                        Text {
                                                            text: modelData.type
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
                                                    z: -1
                                                    onClicked: root.selectedDiskIndex = index
                                                }
                                            }
                                        }

                                        // Expanded volumes for demo disk
                                        Repeater {
                                            model: serviceClient.sources.count > 0
                                                   ? [] : root.demoDisks
                                            delegate: Column {
                                                required property int index
                                                required property var modelData
                                                width: disksColumn.width
                                                visible: root.isDiskExpanded(index)
                                                spacing: 2

                                                Repeater {
                                                    model: modelData.volumes
                                                    delegate: Rectangle {
                                                        required property var modelData
                                                        width: parent.width
                                                        height: 55
                                                        color: Theme.colorListItemAlt
                                                        radius: 4
                                                        RowLayout {
                                                            anchors.fill: parent
                                                            anchors.leftMargin: 40
                                                            anchors.rightMargin: 10
                                                            spacing: 10
                                                            Rectangle {
                                                                width: 16
                                                                height: 16
                                                                radius: 2
                                                                color: "transparent"
                                                                border.width: 1
                                                                border.color: Theme.colorTextGrey
                                                            }
                                                            ColumnLayout {
                                                                Layout.fillWidth: true
                                                                spacing: 2
                                                                RowLayout {
                                                                    spacing: 8
                                                                    Text {
                                                                        text: modelData.name
                                                                        color: Theme.colorTextWhite
                                                                        font.pixelSize: 12
                                                                        font.bold: true
                                                                        font.family: Theme.fontFamily
                                                                    }
                                                                    Text {
                                                                        text: modelData.letter
                                                                        color: Theme.colorAccentBlue
                                                                        font.pixelSize: 12
                                                                        font.family: Theme.fontFamily
                                                                        visible: modelData.letter
                                                                                 .length > 0
                                                                    }
                                                                }
                                                                Text {
                                                                    text: modelData.size
                                                                    color: Theme.colorTextGrey
                                                                    font.pixelSize: 11
                                                                    font.family: Theme.fontFamily
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
                                            model: serviceClient.connections.count > 0
                                                   ? serviceClient.connections
                                                   : root.demoLocations

                                            delegate: Rectangle {
                                                id: locRow
                                                width: locList.width - 8
                                                height: 46
                                                anchors.horizontalCenter: parent
                                                                          ? parent.horizontalCenter
                                                                          : undefined
                                                radius: 4
                                                color: locHover.containsMouse
                                                       ? Theme.colorHover : "transparent"

                                                readonly property string locName:
                                                    (typeof displayName !== "undefined"
                                                     && displayName)
                                                    ? displayName
                                                    : (modelData && modelData.name
                                                       ? modelData.name : "")
                                                readonly property string locPath:
                                                    (typeof connectionId !== "undefined"
                                                     && connectionId)
                                                    ? connectionId
                                                    : (modelData && modelData.path
                                                       ? modelData.path : "")
                                                readonly property bool locDefault:
                                                    (typeof isDefault !== "undefined")
                                                    ? isDefault
                                                    : (modelData && modelData.isDefault)
                                                readonly property bool selected:
                                                    root.selectedLocationIndex === index

                                                required property int index
                                                property var modelData
                                                property string displayName
                                                property string connectionId
                                                property bool isDefault
                                                property bool isAvailable

                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.margins: 8
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
                                                        spacing: 1
                                                        Text {
                                                            text: locRow.locName
                                                            color: Theme.colorTextWhite
                                                            font.pixelSize: 12
                                                            font.bold: true
                                                            font.family: Theme.fontFamily
                                                            elide: Text.ElideRight
                                                            width: parent.width
                                                        }
                                                        Text {
                                                            text: locRow.locPath
                                                            color: Theme.colorTextGrey
                                                            font.pixelSize: 11
                                                            font.family: Theme.fontFamily
                                                            elide: Text.ElideMiddle
                                                            width: parent.width
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

                        // Footer: Cancel + Next (wizard step 1)
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 15
                            Item { Layout.fillWidth: true }

                            AppButton {
                                //% "Cancel"
                                text: qsTrId("aegra.common.cancel")
                                Layout.preferredWidth: 100
                                Layout.preferredHeight: 40
                                onClicked: root.closeWizard()
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
                    }

                    // -------- Step 2: Schedule settings + Options --------
                    BackupWizardStep2 {
                        id: wizardStep2
                        onBackRequested: root.wizardStep = 1
                        onCreateRequested: root.createScheduleFromWizard()
                    }
                }
            }
        }
    }

    // Shared delegate for live Service source model
    Component {
        id: sourceDiskDelegate
        Rectangle {
            required property int index
            required property string displayName
            required property string capacityText
            required property bool isSystem
            required property bool isSelectable
            width: disksColumn.width
            height: 45
            radius: 4
            color: hover.containsMouse ? Theme.colorHover : Theme.colorListItem
            opacity: isSelectable ? 1.0 : 0.55
            readonly property bool selected: root.selectedDiskIndex === index

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
                    color: selected ? Theme.colorAccentBlue : "transparent"
                    border.width: 2
                    border.color: selected ? Theme.colorAccentBlue : Theme.colorTextGrey
                    Text {
                        anchors.centerIn: parent
                        text: selected ? "\u2713" : ""
                        color: "white"
                        font.pixelSize: 12
                        font.bold: true
                    }
                }
                Text {
                    text: "\u25B6"
                    color: Theme.colorTextGrey
                    font.pixelSize: 10
                    Layout.preferredWidth: 15
                }
                DiskIcon {
                    size: 28
                    variant: isSystem ? "system" : "hdd"
                    iconOpacity: isSelectable ? 1.0 : 0.55
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: displayName
                              + (capacityText.length > 0 ? (" (" + capacityText + ")") : "")
                        color: Theme.colorTextWhite
                        font.pixelSize: 13
                        font.bold: true
                        font.family: Theme.fontFamily
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Text {
                        //% "Volume"
                        text: qsTrId("aegra.backup.source.volume")
                        color: Theme.colorTextGrey
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                    }
                }
                Item { Layout.fillWidth: true }
            }
            MouseArea {
                id: hover
                anchors.fill: parent
                hoverEnabled: true
                enabled: isSelectable
                cursorShape: isSelectable ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: root.selectedDiskIndex = index
            }
        }
    }
}
