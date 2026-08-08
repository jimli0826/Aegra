import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Old backup wizard step 2: Schedule settings (left) + Options (right).
Item {
    id: root

    property string frequency: "daily"
    property var daysOfWeek: [1]
    property var daysOfMonth: [1]
    property string timeOfDay: "02:00"
    /// 1 = Full, 2 = Incremental (file_set and volume; Differential not offered for files).
    property int backupType: 1
    /// When true, wizard shows file_set Incremental baseline guidance (no USN/path details).
    property bool filesMode: false
    property bool enableDedup: true
    property bool excludePageHibernation: true
    property bool shutdownWhenComplete: false
    property bool splitEnabled: false
    property int splitSize: 1
    property string splitUnit: "GB"
    property bool encryption: false
    property string password: ""
    property string passwordConfirm: ""
    readonly property int passwordMaxLength: 32
    property string compression: "normal"

    signal backRequested()
    signal createRequested()

    readonly property var hourOptions: [
        "00","01","02","03","04","05","06","07","08","09","10","11",
        "12","13","14","15","16","17","18","19","20","21","22","23"
    ]
    readonly property var minuteOptions: [
        "00","05","10","15","20","25","30","35","40","45","50","55"
    ]

    function pad2(n) {
        return (n < 10 ? "0" : "") + n
    }

    function timeHour() {
        var p = (root.timeOfDay || "02:00").split(":")
        var h = parseInt(p[0], 10)
        if (isNaN(h) || h < 0 || h > 23) h = 2
        return pad2(h)
    }

    function timeMinute() {
        var p = (root.timeOfDay || "02:00").split(":")
        var m = parseInt(p[1], 10)
        if (isNaN(m) || m < 0 || m > 59) m = 0
        m = Math.round(m / 5) * 5
        if (m > 55) m = 55
        return pad2(m)
    }

    function setTimeParts(h, m) {
        root.timeOfDay = (h || "02") + ":" + (m || "00")
    }

    function asIntList(v) {
        if (v === undefined || v === null)
            return []
        if (typeof v === "number")
            return [v]
        var out = []
        for (var i = 0; i < v.length; ++i) {
            var x = parseInt(v[i], 10)
            if (!isNaN(x))
                out.push(x)
        }
        return out
    }

    function isDayOfWeekSelected(d) {
        return asIntList(root.daysOfWeek).indexOf(d) >= 0
    }

    function toggleDayOfWeek(d) {
        var arr = asIntList(root.daysOfWeek)
        var i = arr.indexOf(d)
        if (i >= 0) {
            if (arr.length <= 1)
                return
            arr.splice(i, 1)
        } else {
            arr.push(d)
            arr.sort(function (a, b) { return a - b })
        }
        root.daysOfWeek = arr
    }

    function isDayOfMonthSelected(d) {
        return asIntList(root.daysOfMonth).indexOf(d) >= 0
    }

    function toggleDayOfMonth(d) {
        var arr = asIntList(root.daysOfMonth)
        var i = arr.indexOf(d)
        if (i >= 0) {
            if (arr.length <= 1)
                return
            arr.splice(i, 1)
        } else {
            arr.push(d)
            arr.sort(function (a, b) { return a - b })
        }
        root.daysOfMonth = arr
    }

    function resetDefaults() {
        frequency = "daily"
        daysOfWeek = [1]
        daysOfMonth = [1]
        timeOfDay = "02:00"
        backupType = 1
        enableDedup = true
        excludePageHibernation = true
        shutdownWhenComplete = false
        splitEnabled = false
        splitSize = 1
        splitUnit = "GB"
        encryption = false
        password = ""
        passwordConfirm = ""
        compression = "normal"
        if (hourCombo) {
            var hIdx = hourOptions.indexOf(timeHour())
            hourCombo.currentIndex = hIdx >= 0 ? hIdx : 2
        }
        if (minCombo) {
            var mIdx = minuteOptions.indexOf(timeMinute())
            minCombo.currentIndex = mIdx >= 0 ? mIdx : 0
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 20

            // Left: Schedule settings
            Card {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 2
                Layout.minimumWidth: 360
                //% "Schedule settings"
                title: qsTrId("aegra.backup.schedule_settings")
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.topMargin: 56
                    anchors.margins: 20
                    spacing: 16

                    // Backup type: Full | Incremental (no Differential for product file path)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        Text {
                            Layout.preferredWidth: 100
                            //% "Backup type"
                            text: qsTrId("aegra.backup.column.backup_type")
                            color: Theme.colorTextGrey
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                        }
                        Row {
                            spacing: 8
                            Repeater {
                                model: [
                                    { value: 1, label: qsTrId("aegra.backup.type.full") },
                                    { value: 2, label: qsTrId("aegra.backup.type.incremental") }
                                ]
                                delegate: Rectangle {
                                    required property var modelData
                                    width: Math.max(100, typeLabel.implicitWidth + 28)
                                    height: 34
                                    radius: 4
                                    color: root.backupType === modelData.value
                                           ? Theme.colorAccentBlue
                                           : (typeHover.containsMouse ? Theme.colorButtonHover
                                                                      : Theme.colorButton)
                                    border.width: root.backupType === modelData.value ? 0 : 1
                                    border.color: Theme.colorBorder
                                    Text {
                                        id: typeLabel
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 13
                                        font.family: Theme.fontFamily
                                        font.bold: root.backupType === modelData.value
                                    }
                                    MouseArea {
                                        id: typeHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.backupType = modelData.value
                                    }
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: 116
                        visible: root.filesMode && root.backupType === 2
                        wrapMode: Text.WordWrap
                        //% "Changing the file selection later creates a new full baseline. Incremental runs only when the selection is unchanged and a valid parent exists."
                        text: qsTrId("aegra.backup.file.incremental_baseline_note")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                    }

                    // Frequency
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        Text {
                            Layout.preferredWidth: 100
                            //% "Frequency"
                            text: qsTrId("aegra.backup.column.frequency")
                            color: Theme.colorTextGrey
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                        }
                        Row {
                            spacing: 8
                            Repeater {
                                model: [
                                    { value: "daily", label: qsTrId("aegra.backup.freq.daily") },
                                    { value: "weekly", label: qsTrId("aegra.backup.freq.weekly") },
                                    { value: "monthly", label: qsTrId("aegra.backup.freq.monthly") }
                                ]
                                delegate: Rectangle {
                                    required property var modelData
                                    width: Math.max(90, freqLabel.implicitWidth + 28)
                                    height: 34
                                    radius: 4
                                    color: root.frequency === modelData.value
                                           ? Theme.colorAccentBlue
                                           : (freqHover.containsMouse ? Theme.colorButtonHover
                                                                      : Theme.colorButton)
                                    border.width: root.frequency === modelData.value ? 0 : 1
                                    border.color: Theme.colorBorder
                                    Text {
                                        id: freqLabel
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 13
                                        font.family: Theme.fontFamily
                                        font.bold: root.frequency === modelData.value
                                    }
                                    MouseArea {
                                        id: freqHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.frequency = modelData.value
                                    }
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }

                    // Weekly days
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        visible: root.frequency === "weekly"
                        Text {
                            Layout.preferredWidth: 100
                            //% "Day of week"
                            text: qsTrId("aegra.backup.day_of_week")
                            color: Theme.colorTextGrey
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                        }
                        Row {
                            spacing: 6
                            Repeater {
                                model: [
                                    { value: 1, label: qsTrId("aegra.backup.weekday.mon") },
                                    { value: 2, label: qsTrId("aegra.backup.weekday.tue") },
                                    { value: 3, label: qsTrId("aegra.backup.weekday.wed") },
                                    { value: 4, label: qsTrId("aegra.backup.weekday.thu") },
                                    { value: 5, label: qsTrId("aegra.backup.weekday.fri") },
                                    { value: 6, label: qsTrId("aegra.backup.weekday.sat") },
                                    { value: 7, label: qsTrId("aegra.backup.weekday.sun") }
                                ]
                                delegate: Rectangle {
                                    required property var modelData
                                    width: Math.max(42, wdLabel.implicitWidth + 16)
                                    height: 30
                                    radius: 4
                                    color: root.isDayOfWeekSelected(modelData.value)
                                           ? Theme.colorAccentBlue
                                           : (wdHover.containsMouse ? Theme.colorButtonHover
                                                                    : Theme.colorButton)
                                    border.width: root.isDayOfWeekSelected(modelData.value) ? 0 : 1
                                    border.color: Theme.colorBorder
                                    Text {
                                        id: wdLabel
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                    }
                                    MouseArea {
                                        id: wdHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.toggleDayOfWeek(modelData.value)
                                    }
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }

                    // Monthly days
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        visible: root.frequency === "monthly"
                        Text {
                            Layout.preferredWidth: 100
                            Layout.alignment: Qt.AlignTop
                            //% "Day of month"
                            text: qsTrId("aegra.backup.day_of_month")
                            color: Theme.colorTextGrey
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 7
                            rowSpacing: 6
                            columnSpacing: 6
                            Repeater {
                                model: 31
                                delegate: Rectangle {
                                    required property int index
                                    Layout.preferredWidth: 36
                                    Layout.preferredHeight: 30
                                    radius: 3
                                    color: root.isDayOfMonthSelected(index + 1)
                                           ? Theme.colorAccentBlue
                                           : (domHover.containsMouse ? Theme.colorButtonHover
                                                                     : Theme.colorButton)
                                    border.width: root.isDayOfMonthSelected(index + 1) ? 0 : 1
                                    border.color: Theme.colorBorder
                                    Text {
                                        anchors.centerIn: parent
                                        text: index + 1
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                    }
                                    MouseArea {
                                        id: domHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.toggleDayOfMonth(index + 1)
                                    }
                                }
                            }
                        }
                    }

                    // Time
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        Text {
                            Layout.preferredWidth: 100
                            //% "Time of day"
                            text: qsTrId("aegra.backup.time_of_day")
                            color: Theme.colorTextGrey
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                        }
                        Row {
                            spacing: 8
                            ComboBox {
                                id: hourCombo
                                width: 80
                                height: 34
                                model: root.hourOptions
                                Component.onCompleted: {
                                    var idx = root.hourOptions.indexOf(root.timeHour())
                                    hourCombo.currentIndex = idx >= 0 ? idx : 2
                                }
                                onActivated: root.setTimeParts(hourCombo.currentText,
                                                               minCombo.currentText)
                                background: Rectangle {
                                    color: Theme.colorInput
                                    radius: 4
                                    border.width: 1
                                    border.color: Theme.colorBorder
                                }
                                contentItem: Text {
                                    leftPadding: 10
                                    rightPadding: 22
                                    text: hourCombo.displayText
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                indicator: ComboBoxIndicator { combo: hourCombo }
                                popup: Popup {
                                    y: hourCombo.height + 2
                                    width: hourCombo.width
                                    padding: 2
                                    implicitHeight: Math.min(220, contentItem.implicitHeight + 4)
                                    contentItem: ListView {
                                        clip: true
                                        implicitHeight: contentHeight
                                        model: hourCombo.popup.visible
                                               ? hourCombo.delegateModel : null
                                        currentIndex: hourCombo.highlightedIndex
                                        ScrollIndicator.vertical: ScrollIndicator { }
                                    }
                                    background: Rectangle {
                                        color: Theme.colorPopup
                                        border.color: Theme.colorBorder
                                        radius: 4
                                    }
                                }
                                delegate: ItemDelegate {
                                    width: hourCombo.width
                                    height: 30
                                    contentItem: Text {
                                        text: modelData
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 13
                                        font.family: Theme.fontFamily
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: parent.highlighted ? Theme.colorAccentBlue
                                                                  : "transparent"
                                    }
                                }
                            }
                            Text {
                                text: ":"
                                color: Theme.colorTextWhite
                                font.pixelSize: 16
                                font.bold: true
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            ComboBox {
                                id: minCombo
                                width: 80
                                height: 34
                                model: root.minuteOptions
                                Component.onCompleted: {
                                    var idx = root.minuteOptions.indexOf(root.timeMinute())
                                    minCombo.currentIndex = idx >= 0 ? idx : 0
                                }
                                onActivated: root.setTimeParts(hourCombo.currentText,
                                                               minCombo.currentText)
                                background: Rectangle {
                                    color: Theme.colorInput
                                    radius: 4
                                    border.width: 1
                                    border.color: Theme.colorBorder
                                }
                                contentItem: Text {
                                    leftPadding: 10
                                    rightPadding: 22
                                    text: minCombo.displayText
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                indicator: ComboBoxIndicator { combo: minCombo }
                                popup: Popup {
                                    y: minCombo.height + 2
                                    width: minCombo.width
                                    padding: 2
                                    implicitHeight: Math.min(220, contentItem.implicitHeight + 4)
                                    contentItem: ListView {
                                        clip: true
                                        implicitHeight: contentHeight
                                        model: minCombo.popup.visible
                                               ? minCombo.delegateModel : null
                                        currentIndex: minCombo.highlightedIndex
                                        ScrollIndicator.vertical: ScrollIndicator { }
                                    }
                                    background: Rectangle {
                                        color: Theme.colorPopup
                                        border.color: Theme.colorBorder
                                        radius: 4
                                    }
                                }
                                delegate: ItemDelegate {
                                    width: minCombo.width
                                    height: 30
                                    contentItem: Text {
                                        text: modelData
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 13
                                        font.family: Theme.fontFamily
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        color: parent.highlighted ? Theme.colorAccentBlue
                                                                  : "transparent"
                                    }
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // Right: Options
            Card {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                Layout.minimumWidth: 260
                //% "Options"
                title: qsTrId("aegra.restore.options")
                clip: true

                Flickable {
                    anchors.fill: parent
                    anchors.topMargin: 56
                    anchors.margins: 16
                    contentWidth: width
                    contentHeight: optCol.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    ColumnLayout {
                        id: optCol
                        width: parent.width
                        spacing: 14

                        // Dedup
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Rectangle {
                                width: 18
                                height: 18
                                radius: 3
                                color: root.enableDedup ? Theme.colorAccentBlue : "transparent"
                                border.width: root.enableDedup ? 0 : 1
                                border.color: Theme.colorTextGrey
                                Text {
                                    anchors.centerIn: parent
                                    text: "\u2713"
                                    color: "white"
                                    font.pixelSize: 12
                                    font.bold: true
                                    visible: root.enableDedup
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.enableDedup = !root.enableDedup
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                //% "Enable deduplication"
                                text: qsTrId("aegra.backup.opt.dedup")
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                wrapMode: Text.WordWrap
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.enableDedup = !root.enableDedup
                                }
                            }
                        }

                        // Exclude pagefile
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Rectangle {
                                width: 18
                                height: 18
                                radius: 3
                                color: root.excludePageHibernation
                                       ? Theme.colorAccentBlue : "transparent"
                                border.width: root.excludePageHibernation ? 0 : 1
                                border.color: Theme.colorTextGrey
                                Text {
                                    anchors.centerIn: parent
                                    text: "\u2713"
                                    color: "white"
                                    font.pixelSize: 12
                                    font.bold: true
                                    visible: root.excludePageHibernation
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.excludePageHibernation =
                                                   !root.excludePageHibernation
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                //% "Exclude pagefile / hiberfil / swapfile"
                                text: qsTrId("aegra.backup.opt.exclude_page")
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                wrapMode: Text.WordWrap
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.excludePageHibernation =
                                                   !root.excludePageHibernation
                                }
                            }
                        }

                        // Shutdown
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Rectangle {
                                width: 18
                                height: 18
                                radius: 3
                                color: root.shutdownWhenComplete
                                       ? Theme.colorAccentBlue : "transparent"
                                border.width: root.shutdownWhenComplete ? 0 : 1
                                border.color: Theme.colorTextGrey
                                Text {
                                    anchors.centerIn: parent
                                    text: "\u2713"
                                    color: "white"
                                    font.pixelSize: 12
                                    font.bold: true
                                    visible: root.shutdownWhenComplete
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.shutdownWhenComplete =
                                                   !root.shutdownWhenComplete
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                //% "Shut down when backup completes"
                                text: qsTrId("aegra.backup.opt.shutdown")
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                wrapMode: Text.WordWrap
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.shutdownWhenComplete =
                                                   !root.shutdownWhenComplete
                                }
                            }
                        }

                        // Split image
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                Rectangle {
                                    width: 18
                                    height: 18
                                    radius: 3
                                    color: root.splitEnabled ? Theme.colorAccentBlue
                                                             : "transparent"
                                    border.width: root.splitEnabled ? 0 : 1
                                    border.color: Theme.colorTextGrey
                                    Text {
                                        anchors.centerIn: parent
                                        text: "\u2713"
                                        color: "white"
                                        font.pixelSize: 12
                                        font.bold: true
                                        visible: root.splitEnabled
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.splitEnabled = !root.splitEnabled
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    //% "Split image into fixed-size files"
                                    text: qsTrId("aegra.backup.opt.split")
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                    wrapMode: Text.WordWrap
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.splitEnabled = !root.splitEnabled
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 28
                                spacing: 8
                                visible: root.splitEnabled
                                Rectangle {
                                    width: 56
                                    height: 28
                                    radius: 4
                                    color: Theme.colorInput
                                    border.width: 1
                                    border.color: Theme.colorBorder
                                    TextInput {
                                        anchors.fill: parent
                                        anchors.margins: 6
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 13
                                        font.family: Theme.fontFamily
                                        validator: IntValidator { bottom: 1; top: 1024 }
                                        text: "" + root.splitSize
                                        onTextChanged: {
                                            var n = parseInt(text, 10)
                                            if (!isNaN(n) && n >= 1)
                                                root.splitSize = n
                                        }
                                    }
                                }
                                Rectangle {
                                    width: 56
                                    height: 28
                                    radius: 4
                                    color: Theme.colorButton
                                    border.width: 1
                                    border.color: Theme.colorBorder
                                    Text {
                                        anchors.centerIn: parent
                                        text: root.splitUnit
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.splitUnit =
                                                       (root.splitUnit === "GB") ? "MB" : "GB"
                                    }
                                }
                                Text {
                                    //% "per file"
                                    text: qsTrId("aegra.backup.opt.split_suffix")
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                }
                            }
                        }

                        // Encryption
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Rectangle {
                                width: 18
                                height: 18
                                radius: 3
                                color: root.encryption ? Theme.colorAccentBlue : "transparent"
                                border.width: root.encryption ? 0 : 1
                                border.color: Theme.colorTextGrey
                                Text {
                                    anchors.centerIn: parent
                                    text: "\u2713"
                                    color: "white"
                                    font.pixelSize: 12
                                    font.bold: true
                                    visible: root.encryption
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        root.encryption = !root.encryption
                                        if (!root.encryption) {
                                            root.password = ""
                                            root.passwordConfirm = ""
                                        }
                                    }
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                //% "Encryption (password required)"
                                text: qsTrId("aegra.backup.opt.encryption")
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                wrapMode: Text.WordWrap
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        root.encryption = !root.encryption
                                        if (!root.encryption) {
                                            root.password = ""
                                            root.passwordConfirm = ""
                                        }
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            visible: root.encryption
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 34
                                color: Theme.colorInput
                                radius: 4
                                border.width: 1
                                border.color: Theme.colorBorder
                                TextInput {
                                    id: passwordInput
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    verticalAlignment: Text.AlignVCenter
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                    echoMode: TextInput.Password
                                    maximumLength: root.passwordMaxLength
                                    clip: true
                                    selectByMouse: true
                                    text: root.password
                                    onTextChanged: root.password = text
                                    Text {
                                        anchors.fill: parent
                                        verticalAlignment: Text.AlignVCenter
                                        //% "Password"
                                        text: qsTrId("aegra.backup.opt.password")
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                        visible: !passwordInput.text && !passwordInput.activeFocus
                                    }
                                }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 34
                                color: Theme.colorInput
                                radius: 4
                                border.width: 1
                                border.color: (root.passwordConfirm.length > 0
                                              && root.passwordConfirm !== root.password)
                                             ? Theme.colorAccentRed : Theme.colorBorder
                                TextInput {
                                    id: passwordConfirmInput
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    verticalAlignment: Text.AlignVCenter
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                    echoMode: TextInput.Password
                                    maximumLength: root.passwordMaxLength
                                    clip: true
                                    selectByMouse: true
                                    text: root.passwordConfirm
                                    onTextChanged: root.passwordConfirm = text
                                    Text {
                                        anchors.fill: parent
                                        verticalAlignment: Text.AlignVCenter
                                        //% "Confirm password"
                                        text: qsTrId("aegra.backup.opt.password_confirm")
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                        visible: !passwordConfirmInput.text
                                                 && !passwordConfirmInput.activeFocus
                                    }
                                }
                            }
                        }

                        // Compression
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Text {
                                //% "Compression"
                                text: qsTrId("aegra.backup.opt.compression")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
                            Row {
                                spacing: 8
                                Repeater {
                                    model: [
                                        { value: "none", label: qsTrId("aegra.backup.comp.none") },
                                        { value: "normal", label: qsTrId("aegra.backup.comp.normal") },
                                        { value: "best", label: qsTrId("aegra.backup.comp.best") }
                                    ]
                                    delegate: Rectangle {
                                        required property var modelData
                                        width: Math.max(70, cLabel.implicitWidth + 20)
                                        height: 30
                                        radius: 4
                                        color: root.compression === modelData.value
                                               ? Theme.colorAccentBlue
                                               : (cHover.containsMouse ? Theme.colorButtonHover
                                                                       : Theme.colorButton)
                                        border.width: root.compression === modelData.value ? 0 : 1
                                        border.color: Theme.colorBorder
                                        Text {
                                            id: cLabel
                                            anchors.centerIn: parent
                                            text: modelData.label
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 12
                                            font.family: Theme.fontFamily
                                        }
                                        MouseArea {
                                            id: cHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.compression = modelData.value
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Footer: Back + Create
        RowLayout {
            Layout.fillWidth: true
            spacing: 15
            Item { Layout.fillWidth: true }
            AppButton {
                //% "Back"
                text: qsTrId("aegra.common.back")
                Layout.preferredWidth: 100
                Layout.preferredHeight: 40
                onClicked: root.backRequested()
            }
            AppButton {
                //% "Create"
                text: qsTrId("aegra.common.create")
                Layout.preferredWidth: 140
                Layout.preferredHeight: 40
                primary: true
                onClicked: root.createRequested()
            }
        }
    }
}
