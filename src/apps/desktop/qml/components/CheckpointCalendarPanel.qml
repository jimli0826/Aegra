import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

// Right-side Calendar + Checkpoints panel (old Restore/Mount Select checkpoint drawer).
Item {
    id: root

    /// Controlled by parent only — never assign to open from inside this panel
    /// (writing open breaks the parent's binding and blocks reopening).
    property bool open: false
    /// Dates with backups as "YYYY-MM-DD" (local), from Service recovery points.
    property var backupDates: []
    /// Checkpoints for selectedDate: [{ timeText, backupType, sizeText, fileUuid, ... }]
    property var checkpoints: []
    /// Bump when checkpoints array is replaced so ListView always refreshes.
    property int checkpointsEpoch: 0
    property string selectedDate: ""
    property int displayYear: new Date().getFullYear()
    property int displayMonth: new Date().getMonth() // 0-11
    property bool loading: false

    signal closed()
    signal dateSelected(string dateStr)
    signal checkpointSelected(var item)

    function requestClose() {
        root.closed()
    }

    readonly property var monthNames: [
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    ]
    readonly property var dayNames: ["Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"]

    function pad2(n) {
        return (n < 10 ? "0" : "") + n
    }

    function ymd(y, m, d) {
        return y + "-" + pad2(m + 1) + "-" + pad2(d)
    }

    function hasBackupOn(dateStr) {
        var list = root.backupDates || []
        for (var i = 0; i < list.length; ++i) {
            if (list[i] === dateStr)
                return true
        }
        return false
    }

    function monthCells() {
        var y = root.displayYear
        var m = root.displayMonth
        var first = new Date(y, m, 1)
        var startDow = first.getDay() // 0=Sun
        var daysInMonth = new Date(y, m + 1, 0).getDate()
        var prevDays = new Date(y, m, 0).getDate()
        var today = new Date()
        var todayStr = ymd(today.getFullYear(), today.getMonth(), today.getDate())
        var cells = []
        var total = 42
        for (var i = 0; i < total; ++i) {
            var dayNum
            var inMonth
            var cellY = y
            var cellM = m
            if (i < startDow) {
                dayNum = prevDays - startDow + 1 + i
                inMonth = false
                cellM = m - 1
                if (cellM < 0) {
                    cellM = 11
                    cellY = y - 1
                }
            } else if (i >= startDow + daysInMonth) {
                dayNum = i - startDow - daysInMonth + 1
                inMonth = false
                cellM = m + 1
                if (cellM > 11) {
                    cellM = 0
                    cellY = y + 1
                }
            } else {
                dayNum = i - startDow + 1
                inMonth = true
            }
            var dateStr = ymd(cellY, cellM, dayNum)
            var hasBackup = inMonth && root.hasBackupOn(dateStr)
            cells.push({
                day: dayNum,
                date: dateStr,
                inMonth: inMonth,
                hasBackup: hasBackup,
                enabled: hasBackup,
                selected: root.selectedDate === dateStr,
                isToday: dateStr === todayStr
            })
        }
        return cells
    }

    function previousMonth() {
        if (root.displayMonth === 0) {
            root.displayMonth = 11
            root.displayYear -= 1
        } else {
            root.displayMonth -= 1
        }
    }

    function nextMonth() {
        if (root.displayMonth === 11) {
            root.displayMonth = 0
            root.displayYear += 1
        } else {
            root.displayMonth += 1
        }
    }

    readonly property var cells: monthCells()

    Rectangle {
        anchors.fill: parent
        color: Theme.colorScrim
        opacity: root.open ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 250 } }
        MouseArea {
            anchors.fill: parent
            enabled: root.open
        }
    }

    Rectangle {
        id: panel
        width: Math.max(320, parent.width * 0.5)
        height: parent.height
        property real slideProgress: root.open ? 0 : 1
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
                    //% "Select checkpoint"
                    text: qsTrId("aegra.restore.select_checkpoint_title")
                    color: Theme.colorTextWhite
                    font.pixelSize: 16
                    font.bold: true
                    font.family: Theme.fontFamily
                }
                Item { Layout.fillWidth: true }
                Button {
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 28
                    text: "\u2715"
                    background: Rectangle {
                        color: parent.hovered ? Theme.colorHover : "transparent"
                        radius: 4
                    }
                    contentItem: Text {
                        text: parent.text
                        color: Theme.colorTextWhite
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: root.requestClose()
                }
            }

            // Calendar card
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 300
                Layout.minimumHeight: 260
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
                            //% "Calendar"
                            text: qsTrId("aegra.restore.calendar")
                            color: Theme.colorTextWhite
                            font.pixelSize: 14
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            text: "\u2039"
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 28
                            background: Rectangle {
                                color: parent.hovered ? Theme.colorHover : "transparent"
                                radius: 4
                            }
                            contentItem: Text {
                                text: parent.text
                                color: Theme.colorTextWhite
                                font.pixelSize: 16
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: root.previousMonth()
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: root.monthNames[root.displayMonth] + " " + root.displayYear
                            color: Theme.colorTextWhite
                            font.pixelSize: 14
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Button {
                            text: "\u203A"
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 28
                            background: Rectangle {
                                color: parent.hovered ? Theme.colorHover : "transparent"
                                radius: 4
                            }
                            contentItem: Text {
                                text: parent.text
                                color: Theme.colorTextWhite
                                font.pixelSize: 16
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: root.nextMonth()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Repeater {
                            model: root.dayNames
                            delegate: Text {
                                required property string modelData
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: modelData
                                color: Theme.colorTextGrey
                                font.pixelSize: 10
                                font.family: Theme.fontFamily
                            }
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        columns: 7
                        rowSpacing: 2
                        columnSpacing: 2
                        Repeater {
                            model: root.cells
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumHeight: 26
                                radius: 4
                                property bool canSelect: modelData && modelData.enabled === true
                                property bool isSelected: modelData && modelData.selected === true
                                property bool hasBackup: modelData && modelData.hasBackup === true
                                property bool inMonth: modelData && modelData.inMonth === true
                                color: {
                                    if (isSelected)
                                        return Theme.colorAccentBlue
                                    if (dayMouse.containsMouse && canSelect)
                                        return Theme.colorHover
                                    if (hasBackup && inMonth)
                                        return Theme.colorCalendarHasBackup
                                    return "transparent"
                                }
                                border.width: (modelData && modelData.isToday && !isSelected) ? 1 : 0
                                border.color: Theme.colorAccentBlue
                                opacity: inMonth ? 1.0 : 0.45
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData ? modelData.day : ""
                                    color: {
                                        if (!inMonth)
                                            return Theme.colorTextDim
                                        if (isSelected)
                                            return Theme.colorTextWhite
                                        if (!hasBackup)
                                            return Theme.colorCalendarMuted
                                        return Theme.colorTextWhite
                                    }
                                    font.pixelSize: 12
                                    font.bold: hasBackup && inMonth
                                    font.family: Theme.fontFamily
                                }
                                Rectangle {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 2
                                    width: 3
                                    height: 3
                                    radius: 1.5
                                    visible: hasBackup && inMonth && !isSelected
                                    color: Theme.colorAccentBlue
                                }
                                MouseArea {
                                    id: dayMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: canSelect
                                    cursorShape: canSelect ? Qt.PointingHandCursor : Qt.ArrowCursor
                                    onClicked: {
                                        // Do not write selectedDate here — parent owns it via
                                        // binding; only emit so checkpoints load once.
                                        if (modelData && canSelect)
                                            root.dateSelected(modelData.date)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Checkpoints card
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
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
                            //% "Checkpoints"
                            text: qsTrId("aegra.restore.checkpoints")
                            color: Theme.colorTextWhite
                            font.pixelSize: 14
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }
                    ListView {
                        id: cpList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 4
                        // Depend on epoch so a fresh QVariantList always reloads the view.
                        model: {
                            var _e = root.checkpointsEpoch
                            return root.checkpoints
                        }
                        delegate: Rectangle {
                            required property var modelData
                            width: cpList.width
                            height: 34
                            radius: 4
                            color: cpMouse.containsMouse ? Theme.colorHover : Theme.colorListItem
                            MouseArea {
                                id: cpMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.checkpointSelected(modelData)
                                    root.requestClose()
                                }
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8
                                Text {
                                    Layout.preferredWidth: 72
                                    text: modelData.timeText || ""
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                }
                                Text {
                                    text: modelData.backupType || ""
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: modelData.sizeText || ""
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 11
                                    font.family: Theme.fontFamily
                                }
                            }
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: cpList.count === 0 && !root.loading
                            width: parent.width - 24
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: root.selectedDate.length === 0
                                  ? qsTrId("aegra.restore.select_date_with_backups")
                                  : qsTrId("aegra.restore.no_recovery_points_on_date")
                            color: Theme.colorTextGrey
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                        }
                    }
                }
            }
        }
    }
}
