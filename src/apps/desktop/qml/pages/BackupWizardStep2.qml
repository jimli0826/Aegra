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
    /// One or more HH:mm values (max 8). Always a real JS array of "HH:mm" strings.
    property var timesOfDay: ["02:00"]
    readonly property int maxTimesOfDay: 8
    /// Minutes between any two times (same calendar day, including wrap past midnight).
    readonly property int minTimeGapMinutes: 30
    /// False while hydrating from a schedule so ComboBox init cannot invent 00:00.
    property bool timesUiReady: true
    /// When true, wizard shows file_set Incremental metadata-signature guidance.
    property bool filesMode: false
    /// Schedule edit: persist trigger; lock backup options that were frozen at create.
    property bool editing: false
    /// Bumped on every Add/close so this page returns to factory defaults.
    property int draftGeneration: 0
    onDraftGenerationChanged: resetDefaults()
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

    function normalizeTimeLabel(value) {
        var p = ("" + (value || "02:00")).split(":")
        var h = parseInt(p[0], 10)
        var m = parseInt(p[1], 10)
        if (isNaN(h) || h < 0 || h > 23) h = 2
        if (isNaN(m) || m < 0 || m > 59) m = 0
        m = Math.round(m / 5) * 5
        if (m > 55) m = 55
        return pad2(h) + ":" + pad2(m)
    }

    function timeToMinutes(value) {
        var label = root.normalizeTimeLabel(value)
        var p = label.split(":")
        return parseInt(p[0], 10) * 60 + parseInt(p[1], 10)
    }

    function minutesToLabel(total) {
        var t = ((total % 1440) + 1440) % 1440
        return pad2(Math.floor(t / 60)) + ":" + pad2(t % 60)
    }

    /// Empty string if ok; otherwise a qsTrId key.
    function timesValidationError(list) {
        var values = []
        var seen = {}
        var i
        for (i = 0; i < (list || []).length; ++i) {
            var mins = root.timeToMinutes(list[i])
            if (seen[mins])
                return "aegra.backup.time.duplicate"
            seen[mins] = true
            values.push(mins)
        }
        if (values.length <= 1)
            return ""
        values.sort(function (a, b) { return a - b })
        for (i = 1; i < values.length; ++i) {
            if (values[i] - values[i - 1] < root.minTimeGapMinutes)
                return "aegra.backup.time.min_interval"
        }
        var wrap = (values[0] + 1440) - values[values.length - 1]
        if (wrap < root.minTimeGapMinutes)
            return "aegra.backup.time.min_interval"
        return ""
    }

    function isValidTimesList(list) {
        return root.timesValidationError(list).length === 0
    }

    function timeHourAt(index) {
        var list = root.timesOfDay || []
        return root.normalizeTimeLabel(list[index] || "02:00").split(":")[0]
    }

    function timeMinuteAt(index) {
        var list = root.timesOfDay || []
        return root.normalizeTimeLabel(list[index] || "02:00").split(":")[1]
    }

    function setTimeAt(index, h, m) {
        if (!root.timesUiReady)
            return
        var list = root.normalizedTimesArray(root.timesOfDay)
        if (index < 0 || index >= list.length)
            return
        var next = list.slice()
        next[index] = root.normalizeTimeLabel((h || "02") + ":" + (m || "00"))
        var err = root.timesValidationError(next)
        if (err.length > 0) {
            if (typeof serviceClient !== "undefined" && serviceClient)
                serviceClient.showToast(qsTrId(err), true)
            root.timesOfDay = list
            return
        }
        root.timesOfDay = next
    }

    /// Accept only real HH:mm tokens; never treat a string as a char array.
    function normalizedTimesArray(value) {
        var raw = []
        if (value === undefined || value === null) {
            // keep empty
        } else if (typeof value === "string") {
            raw = value.split(/[,;]/)
        } else if (typeof value.length === "number") {
            for (var i = 0; i < value.length; ++i) {
                var el = value[i]
                // Skip non-string/number garbage from QVariant quirks.
                if (el === undefined || el === null)
                    continue
                if (typeof el === "object")
                    continue
                raw.push(el)
            }
        }
        var times = []
        var seen = {}
        for (var ci = 0; ci < raw.length; ++ci) {
            var token = ("" + raw[ci]).trim()
            if (!/^\d{1,2}:\d{2}$/.test(token))
                continue
            var label = root.normalizeTimeLabel(token)
            var mins = root.timeToMinutes(label)
            if (seen[mins])
                continue
            seen[mins] = true
            times.push(label)
        }
        times.sort(function (a, b) {
            return root.timeToMinutes(a) - root.timeToMinutes(b)
        })
        if (times.length === 0)
            times = ["02:00"]
        if (times.length > root.maxTimesOfDay)
            times = times.slice(0, root.maxTimesOfDay)
        return times
    }

    function findAvailableTimeLabel(existing) {
        var occupied = {}
        var i
        for (i = 0; i < (existing || []).length; ++i)
            occupied[root.timeToMinutes(existing[i])] = true
        var start = 2 * 60
        if ((existing || []).length > 0) {
            var last = root.timeToMinutes(existing[existing.length - 1])
            start = (last + root.minTimeGapMinutes) % 1440
            start = Math.ceil(start / 5) * 5
            if (start >= 1440)
                start = 0
        }
        for (var step = 0; step < 1440; step += 5) {
            var candidate = (start + step) % 1440
            candidate = Math.floor(candidate / 5) * 5
            var trial = (existing || []).slice()
            trial.push(root.minutesToLabel(candidate))
            if (root.isValidTimesList(trial))
                return root.minutesToLabel(candidate)
        }
        return ""
    }

    function addTimeOfDay() {
        var list = (root.timesOfDay || []).slice()
        if (list.length >= root.maxTimesOfDay)
            return
        var label = root.findAvailableTimeLabel(list)
        if (label.length === 0) {
            if (typeof serviceClient !== "undefined" && serviceClient)
                serviceClient.showToast(qsTrId("aegra.backup.time.no_slot"), true)
            return
        }
        list.push(label)
        root.timesOfDay = list
    }

    function removeTimeOfDay(index) {
        var list = (root.timesOfDay || []).slice()
        if (list.length <= 1 || index < 0 || index >= list.length)
            return
        list.splice(index, 1)
        root.timesOfDay = list
    }

    /// Joined HH:mm list for ServiceClient (comma-separated).
    function selectedTimeOfDay() {
        var list = root.normalizedTimesArray(root.timesOfDay)
        return list.join(",")
    }

    function validateTimesOfDay() {
        return root.timesValidationError(root.normalizedTimesArray(root.timesOfDay))
    }

    function selectedFrequency() {
        if (root.frequency === "weekly")
            return "weekly"
        if (root.frequency === "monthly")
            return "monthly"
        return "daily"
    }

    function optionFill(checked) {
        if (!checked)
            return "transparent"
        return root.editing ? Theme.colorButtonDisabled : Theme.colorAccentBlue
    }

    function optionBorder(checked) {
        if (root.editing)
            return Theme.colorButtonDisabledText
        return checked ? Theme.colorAccentBlue : Theme.colorTextGrey
    }

    function optionMarkColor() {
        return root.editing ? Theme.colorButtonDisabledText : Theme.colorOnAccent
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

    function applyFromSchedule(item) {
        if (!item)
            return
        frequency = (item.frequency || "daily").toString().toLowerCase()
        if (frequency !== "daily" && frequency !== "weekly" && frequency !== "monthly")
            frequency = "daily"
        // Prefer joined timeOfDay string — stable across QML modelData type quirks.
        // timesOfDay array is only used when every element is a full "H:MM"/"HH:MM" token.
        root.timesUiReady = false
        var source = item.timeOfDay
        if (item.timesOfDay !== undefined && item.timesOfDay !== null
                && typeof item.timesOfDay !== "string"
                && typeof item.timesOfDay.length === "number" && item.timesOfDay.length > 0) {
            var allOk = true
            var joined = []
            for (var ti = 0; ti < item.timesOfDay.length; ++ti) {
                var el = item.timesOfDay[ti]
                if (el === undefined || el === null || typeof el === "object") {
                    allOk = false
                    break
                }
                var tok = ("" + el).trim()
                if (!/^\d{1,2}:\d{2}$/.test(tok)) {
                    allOk = false
                    break
                }
                joined.push(tok)
            }
            if (allOk)
                source = joined.join(",")
        }
        timesOfDay = root.normalizedTimesArray(source)
        if (typeof serviceClient !== "undefined" && serviceClient
                && typeof serviceClient.logScheduleEdit === "function") {
            serviceClient.logScheduleEdit(
                        "applyTimes rawTimeOfDay=[" + (item.timeOfDay || "")
                        + "] rawTimesType=" + (typeof item.timesOfDay)
                        + " source=[" + source + "] result=[" + timesOfDay.join(",") + "]")
        }
        Qt.callLater(function () { root.timesUiReady = true })
        enableDedup = item.deduplicationEnabled !== false
        excludePageHibernation = item.excludePageAndHibernation !== false
        encryption = !!item.encryptionEnabled
        password = ""
        passwordConfirm = ""
        var mask = parseInt(item.weekdayMask, 10)
        if (isNaN(mask) || mask <= 0)
            mask = 2
        var days = []
        for (var bit = 0; bit <= 6; ++bit) {
            if ((mask & (1 << bit)) !== 0)
                days.push(bit === 0 ? 7 : bit)
        }
        daysOfWeek = days.length > 0 ? days : [1]
        var monthMask = parseInt(item.dayOfMonthMask, 10)
        if (isNaN(monthMask) || monthMask <= 0)
            monthMask = 1
        var monthDays = []
        for (var monthBit = 0; monthBit < 31; ++monthBit) {
            if ((monthMask & (1 << monthBit)) !== 0)
                monthDays.push(monthBit + 1)
        }
        daysOfMonth = monthDays.length > 0 ? monthDays : [1]
    }

    function resetDefaults() {
        frequency = "daily"
        daysOfWeek = [1]
        daysOfMonth = [1]
        timesUiReady = false
        timesOfDay = ["02:00"]
        Qt.callLater(function () { root.timesUiReady = true })
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
            spacing: 12

            // Left: Schedule settings
            Card {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 2
                Layout.minimumWidth: 360
                //% "Schedule settings"
                title: qsTrId("aegra.backup.schedule_settings")
                clip: true

                ScrollView {
                    anchors.fill: parent
                    anchors.topMargin: 54
                    anchors.bottomMargin: 12
                    anchors.leftMargin: 20
                    anchors.rightMargin: 6
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    ScrollBar.vertical.policy: ScrollBar.AsNeeded

                    ColumnLayout {
                        width: Math.max(300, parent.width - 8)
                        spacing: 16

                        Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: root.filesMode
                        //% "Each run is incremental. The first backup, or a run with no usable parent, becomes a full backup automatically."
                        text: qsTrId("aegra.backup.file.incremental_default_note")
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

                    // Time of day (one or more)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        Text {
                            Layout.preferredWidth: 100
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 8
                            //% "Time of day"
                            text: qsTrId("aegra.backup.time_of_day")
                            color: Theme.colorTextGrey
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Repeater {
                                // Object model keeps row identity stable when appending a time,
                                // so existing ComboBoxes are not recreated to default 02:00.
                                model: root.timesOfDay.length
                                delegate: Row {
                                    spacing: 8
                                    required property int index
                                    ComboBox {
                                        id: hourCombo
                                        width: 80
                                        height: 34
                                        model: root.hourOptions
                                        // Do not bind currentIndex: user selection breaks that
                                        // binding and can leave a stale default after Add time.
                                        function syncFromModel() {
                                            var idx = root.hourOptions.indexOf(
                                                        root.timeHourAt(index))
                                            if (idx < 0)
                                                idx = 2
                                            if (hourCombo.currentIndex !== idx)
                                                hourCombo.currentIndex = idx
                                        }
                                        Component.onCompleted: Qt.callLater(syncFromModel)
                                        Connections {
                                            target: root
                                            function onTimesOfDayChanged() {
                                                Qt.callLater(hourCombo.syncFromModel)
                                            }
                                        }
                                        // Signal arg must not be named "index" (shadows row index).
                                        onActivated: function (activatedIndex) {
                                            if (!root.timesUiReady)
                                                return
                                            root.setTimeAt(
                                                index,
                                                root.hourOptions[activatedIndex] || "02",
                                                root.timeMinuteAt(index))
                                        }
                                        background: Rectangle {
                                            color: Theme.colorInput
                                            radius: 8
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
                                                radius: 8
                                            }
                                        }
                                        delegate: ItemDelegate {
                                            width: hourCombo.width
                                            height: 30
                                            hoverEnabled: true
                                            highlighted: hourCombo.highlightedIndex === index
                                            contentItem: Text {
                                                text: modelData
                                                color: Theme.colorTextWhite
                                                font.pixelSize: 13
                                                font.family: Theme.fontFamily
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                radius: 4
                                                color: (parent.hovered || parent.highlighted)
                                                       ? Theme.colorHover : "transparent"
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
                                        function syncFromModel() {
                                            var idx = root.minuteOptions.indexOf(
                                                        root.timeMinuteAt(index))
                                            if (idx < 0)
                                                idx = 0
                                            if (minCombo.currentIndex !== idx)
                                                minCombo.currentIndex = idx
                                        }
                                        Component.onCompleted: Qt.callLater(syncFromModel)
                                        Connections {
                                            target: root
                                            function onTimesOfDayChanged() {
                                                Qt.callLater(minCombo.syncFromModel)
                                            }
                                        }
                                        onActivated: function (activatedIndex) {
                                            if (!root.timesUiReady)
                                                return
                                            root.setTimeAt(
                                                index,
                                                root.timeHourAt(index),
                                                root.minuteOptions[activatedIndex] || "00")
                                        }
                                        background: Rectangle {
                                            color: Theme.colorInput
                                            radius: 8
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
                                                radius: 8
                                            }
                                        }
                                        delegate: ItemDelegate {
                                            width: minCombo.width
                                            height: 30
                                            hoverEnabled: true
                                            highlighted: minCombo.highlightedIndex === index
                                            contentItem: Text {
                                                text: modelData
                                                color: Theme.colorTextWhite
                                                font.pixelSize: 13
                                                font.family: Theme.fontFamily
                                                horizontalAlignment: Text.AlignHCenter
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            background: Rectangle {
                                                radius: 4
                                                color: (parent.hovered || parent.highlighted)
                                                       ? Theme.colorHover : "transparent"
                                            }
                                        }
                                    }
                                    AppButton {
                                        //% "Remove"
                                        text: qsTrId("aegra.common.remove")
                                        visible: (root.timesOfDay || []).length > 1
                                        height: 34
                                        onClicked: root.removeTimeOfDay(index)
                                    }
                                }
                            }
                            AppButton {
                                //% "Add time"
                                text: qsTrId("aegra.backup.add_time")
                                visible: (root.timesOfDay || []).length < root.maxTimesOfDay
                                Layout.preferredHeight: 34
                                onClicked: root.addTimeOfDay()
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                    }
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

                ScrollView {
                    anchors.fill: parent
                    anchors.topMargin: 54
                    anchors.bottomMargin: 12
                    anchors.leftMargin: 20
                    anchors.rightMargin: 6
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    ScrollBar.vertical.policy: ScrollBar.AsNeeded

                    ColumnLayout {
                        id: optCol
                        width: Math.max(220, parent.width - 8)
                        spacing: 14

                        // Volume Set single-chunk DEDUP only (ADR-0022); hidden for file_set.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            visible: !root.filesMode
                            Rectangle {
                                width: 18
                                height: 18
                                radius: 3
                                color: root.optionFill(root.enableDedup)
                                border.width: root.enableDedup && !root.editing ? 0 : 1
                                border.color: root.optionBorder(root.enableDedup)
                                Text {
                                    anchors.centerIn: parent
                                    text: "\u2713"
                                    color: root.optionMarkColor()
                                    font.pixelSize: 12
                                    font.bold: true
                                    visible: root.enableDedup
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: !root.editing
                                    cursorShape: root.editing ? Qt.ArrowCursor
                                                              : Qt.PointingHandCursor
                                    onClicked: root.enableDedup = !root.enableDedup
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    //% "Enable volume chunk deduplication"
                                    text: qsTrId("aegra.backup.opt.dedup")
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                    wrapMode: Text.WordWrap
                                    MouseArea {
                                        anchors.fill: parent
                                        enabled: !root.editing
                                        cursorShape: root.editing ? Qt.ArrowCursor
                                                                  : Qt.PointingHandCursor
                                        onClicked: root.enableDedup = !root.enableDedup
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    //% "Same-chunk only within each volume backup; not global or cross-backup"
                                    text: qsTrId("aegra.backup.opt.dedup_hint")
                                    color: Theme.colorTextDim
                                    font.pixelSize: 11
                                    font.family: Theme.fontFamily
                                    wrapMode: Text.WordWrap
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
                                color: root.optionFill(root.excludePageHibernation)
                                border.width: root.excludePageHibernation && !root.editing ? 0 : 1
                                border.color: root.optionBorder(root.excludePageHibernation)
                                Text {
                                    anchors.centerIn: parent
                                    text: "\u2713"
                                    color: root.optionMarkColor()
                                    font.pixelSize: 12
                                    font.bold: true
                                    visible: root.excludePageHibernation
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: !root.editing
                                    cursorShape: root.editing ? Qt.ArrowCursor
                                                              : Qt.PointingHandCursor
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
                                    enabled: !root.editing
                                    cursorShape: root.editing ? Qt.ArrowCursor
                                                              : Qt.PointingHandCursor
                                    onClicked: root.excludePageHibernation =
                                                   !root.excludePageHibernation
                                }
                            }
                        }

                        // Shutdown
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            visible: !root.editing
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
                            visible: !root.editing
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
                                    border.color: splitInput.activeFocus ? Theme.colorAccentBlue : Theme.colorBorder
                                    TextInput {
                                        id: splitInput
                                        anchors.fill: parent
                                        anchors.margins: 6
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 13
                                        font.family: Theme.fontFamily
                                        activeFocusOnTab: true
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
                                color: root.optionFill(root.encryption)
                                border.width: root.encryption && !root.editing ? 0 : 1
                                border.color: root.optionBorder(root.encryption)
                                Text {
                                    anchors.centerIn: parent
                                    text: "\u2713"
                                    color: root.optionMarkColor()
                                    font.pixelSize: 12
                                    font.bold: true
                                    visible: root.encryption
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: !root.editing
                                    cursorShape: root.editing ? Qt.ArrowCursor
                                                              : Qt.PointingHandCursor
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
                                    enabled: !root.editing
                                    cursorShape: root.editing ? Qt.ArrowCursor
                                                              : Qt.PointingHandCursor
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

                        Text {
                            Layout.fillWidth: true
                            visible: root.editing && root.encryption
                            wrapMode: Text.WordWrap
                            //% "Password is set and cannot be changed."
                            text: qsTrId("aegra.backup.opt.password_frozen")
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            visible: root.encryption && !root.editing
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 34
                                color: Theme.colorInput
                                radius: 4
                                border.width: 1
                                border.color: passwordInput.activeFocus ? Theme.colorAccentBlue : Theme.colorBorder
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
                                    activeFocusOnTab: true
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
                                             ? Theme.colorAccentRed
                                             : (passwordConfirmInput.activeFocus ? Theme.colorAccentBlue : Theme.colorBorder)
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
                                    activeFocusOnTab: true
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
                            visible: !root.editing
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
                text: root.editing ? qsTrId("aegra.common.save")
                                   : qsTrId("aegra.common.create")
                Layout.preferredWidth: 140
                Layout.preferredHeight: 40
                primary: true
                onClicked: root.createRequested()
            }
        }
    }
}
