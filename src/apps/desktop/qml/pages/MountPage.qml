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
    property string preferredDriveLetter: ""

    readonly property var driveLetterModel: [
        { label: qsTrId("aegra.mount.drive_letter_auto"), value: "" },
        { label: "D:", value: "D" },
        { label: "E:", value: "E" },
        { label: "F:", value: "F" },
        { label: "G:", value: "G" },
        { label: "H:", value: "H" },
        { label: "I:", value: "I" },
        { label: "J:", value: "J" },
        { label: "K:", value: "K" },
        { label: "L:", value: "L" },
        { label: "M:", value: "M" },
        { label: "N:", value: "N" },
        { label: "O:", value: "O" },
        { label: "P:", value: "P" },
        { label: "Q:", value: "Q" },
        { label: "R:", value: "R" },
        { label: "S:", value: "S" },
        { label: "T:", value: "T" },
        { label: "U:", value: "U" },
        { label: "V:", value: "V" },
        { label: "W:", value: "W" },
        { label: "X:", value: "X" },
        { label: "Y:", value: "Y" },
        { label: "Z:", value: "Z" }
    ]

    function todayYmd() {
        var d = new Date()
        var m = d.getMonth() + 1
        var day = d.getDate()
        return d.getFullYear() + "-"
               + (m < 10 ? "0" : "") + m + "-"
               + (day < 10 ? "0" : "") + day
    }

    function syncDriveLetterIndex() {
        var want = root.preferredDriveLetter || ""
        for (var i = 0; i < root.driveLetterModel.length; ++i) {
            if ((root.driveLetterModel[i].value || "") === want) {
                driveLetterCombo.currentIndex = i
                return
            }
        }
        driveLetterCombo.currentIndex = 0
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

        // Main: Source + Mounted (left) | Options (right) — old 2/3 | 1/3
        RowLayout {
            id: mainSplitRow
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // LEFT: Source disks + Mounted (vertical splitter)
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
                                onClicked: root.checkpointPanelOpen = true
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: root.selectedCheckpointId.length > 0
                            //% "Selected:"
                            text: qsTrId("aegra.restore.selected_label")
                                  + " " + root.selectedCheckpointId
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
                                //% "Select a checkpoint to view source disks"
                                text: qsTrId("aegra.mount.source_empty")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
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
                            }
                        }

                        // Column headers (old: Drive(s) | Disk | Image | Size)
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10
                            spacing: 8
                            Rectangle {
                                Layout.preferredWidth: 28
                                Layout.preferredHeight: 18
                                color: "transparent"
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 18
                                    height: 18
                                    radius: 3
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Theme.colorTextGrey
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
                        }
                    }
                }
            }

            // Horizontal drag handle: resize left area vs Options
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

                // Expanded
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

                    // Drive letter
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
                            Component.onCompleted: root.syncDriveLetterIndex()
                            onActivated: function(index) {
                                if (index < 0 || index >= root.driveLetterModel.length)
                                    return
                                root.preferredDriveLetter =
                                        root.driveLetterModel[index].value || ""
                            }

                            background: Rectangle {
                                color: Theme.colorInput
                                radius: 4
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
                                    radius: 4
                                }
                            }
                            delegate: ItemDelegate {
                                width: driveLetterCombo.width
                                height: 30
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
                                    color: parent.highlighted
                                           ? Theme.colorAccentBlue : "transparent"
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

        // Footer: Unmount + Mount (old layout — bottom right, not inside Options)
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
                enabled: false
            }
            AppButton {
                //% "Mount"
                text: qsTrId("aegra.nav.mount")
                Layout.preferredWidth: 140
                Layout.preferredHeight: 40
                enabled: false
            }
        }
    }

    CheckpointCalendarPanel {
        anchors.fill: parent
        z: 2000
        open: root.checkpointPanelOpen
        backupDates: [root.todayYmd()]
        onClosed: root.checkpointPanelOpen = false
        onCheckpointSelected: function(item) {
            root.selectedCheckpointId = (item && item.timeText)
                                        ? item.timeText : "selected"
        }
    }
}
