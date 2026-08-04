import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Visual baseline: backup/src/gui RestorePage layout (no Service wiring yet).
Item {
    id: root
    //% "Restore"
    Accessible.name: qsTrId("aegra.nav.restore")

    property bool optionsCollapsed: true
    property bool checkpointPanelOpen: false
    property bool preserveSignature: true
    property bool autoExtend: true
    property real sourceTargetRatio: 0.45
    property real optionsPaneRatio: 0.30
    property string selectedCheckpointId: ""

    function todayYmd() {
        var d = new Date()
        var m = d.getMonth() + 1
        var day = d.getDate()
        return d.getFullYear() + "-"
               + (m < 10 ? "0" : "") + m + "-"
               + (day < 10 ? "0" : "") + day
    }

    readonly property var demoTargetDisks: [
        {
            name: "Disk 0",
            style: "Basic (GPT)",
            size: "20.0 GB",
            isSystem: false,
            volumes: [
                { letter: "E:", size: "20.0 GB", fs: "NTFS" }
            ]
        },
        {
            name: "Disk 1",
            style: "Basic (GPT)",
            size: "30.0 GB",
            isSystem: true,
            volumes: [
                { letter: "C:", size: "19.7 GB", fs: "NTFS" }
            ]
        }
    ]

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

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: root.optionsCollapsed
                                       ? 1
                                       : Math.round(1000 * (1.0 - root.optionsPaneRatio))
                Layout.minimumWidth: 280
                spacing: 0

                // Source disks
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
                                onClicked: root.checkpointPanelOpen = true
                            }
                        }

                        Text {
                            //% "Selected:"
                            text: qsTrId("aegra.restore.selected_label")
                                  + (root.selectedCheckpointId.length > 0
                                     ? (" " + root.selectedCheckpointId) : "")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 32
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                //% "Select a checkpoint to view source disks"
                                text: qsTrId("aegra.restore.select_checkpoint_source")
                                color: Theme.colorTextGrey
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                visible: root.selectedCheckpointId.length === 0
                            }
                        }
                    }
                }

                // Splitter handle
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 6
                    color: "transparent"
                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width
                        height: 1
                        color: Theme.colorBorder
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.SplitVCursor
                        property real startY: 0
                        property real startRatio: 0.45
                        onPressed: function(mouse) {
                            startY = mouse.y
                            startRatio = root.sourceTargetRatio
                        }
                        onPositionChanged: function(mouse) {
                            if (!pressed)
                                return
                            const dy = mouse.y - startY
                            const h = parent.parent.height
                            if (h <= 0)
                                return
                            let r = startRatio + dy / h
                            if (r < 0.2) r = 0.2
                            if (r > 0.8) r = 0.8
                            root.sourceTargetRatio = r
                        }
                    }
                }

                // Target disks
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
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 8
                            model: root.demoTargetDisks
                            delegate: Rectangle {
                                required property var modelData
                                width: ListView.view.width
                                height: 64
                                radius: 4
                                color: Theme.colorListItem
                                border.width: 1
                                border.color: Theme.colorBorder

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 12

                                    DiskIcon {
                                        size: 28
                                        variant: modelData.isSystem ? "system" : "hdd"
                                    }
                                    ColumnLayout {
                                        Layout.preferredWidth: 120
                                        spacing: 2
                                        Text {
                                            text: modelData.name
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 13
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                        }
                                        Text {
                                            text: modelData.style + "\n" + modelData.size
                                            color: Theme.colorTextGrey
                                            font.pixelSize: 11
                                            font.family: Theme.fontFamily
                                        }
                                    }
                                    Row {
                                        Layout.fillWidth: true
                                        spacing: 6
                                        Repeater {
                                            model: modelData.volumes
                                            delegate: Rectangle {
                                                required property var modelData
                                                width: 140
                                                height: 40
                                                radius: 3
                                                color: Theme.colorInput
                                                border.width: 1
                                                border.color: Theme.colorBorder
                                                Column {
                                                    anchors.centerIn: parent
                                                    spacing: 1
                                                    Text {
                                                        anchors.horizontalCenter: parent.horizontalCenter
                                                        text: modelData.letter
                                                        color: Theme.colorTextWhite
                                                        font.pixelSize: 12
                                                        font.bold: true
                                                        font.family: Theme.fontFamily
                                                    }
                                                    Text {
                                                        anchors.horizontalCenter: parent.horizontalCenter
                                                        text: modelData.size + " " + modelData.fs
                                                        color: Theme.colorTextGrey
                                                        font.pixelSize: 10
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
            }

            // Options pane
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: root.optionsCollapsed ? 36
                                       : Math.round(1000 * root.optionsPaneRatio)
                Layout.minimumWidth: root.optionsCollapsed ? 36 : 200
                color: Theme.colorCard
                radius: 4
                border.width: 1
                border.color: Theme.colorBorder
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: root.optionsCollapsed ? 4 : 12
                    spacing: 12
                    visible: !root.optionsCollapsed

                    RowLayout {
                        Layout.fillWidth: true
                        Rectangle {
                            width: 3
                            height: 16
                            color: Theme.colorAccentBlue
                        }
                        Text {
                            //% "Options"
                            text: qsTrId("aegra.restore.options")
                            color: Theme.colorTextWhite
                            font.pixelSize: 14
                            font.bold: true
                            font.family: Theme.fontFamily
                            Layout.fillWidth: true
                        }
                        Text {
                            text: "\uE76C"
                            font.family: "Segoe MDL2 Assets"
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.optionsCollapsed = true
                            }
                        }
                    }

                    CheckBox {
                        id: preserveBox
                        //% "Preserve disk signature"
                        text: qsTrId("aegra.restore.preserve_signature")
                        checked: root.preserveSignature
                        onCheckedChanged: root.preserveSignature = checked
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
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            leftPadding: preserveBox.indicator.width + 8
                            wrapMode: Text.WordWrap
                        }
                    }
                    Text {
                        //% "Keeps the original disk signature when supported"
                        text: qsTrId("aegra.restore.preserve_signature_hint")
                        color: Theme.colorTextDim
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    CheckBox {
                        id: extendBox
                        //% "Auto-extend last partition"
                        text: qsTrId("aegra.restore.auto_extend")
                        checked: root.autoExtend
                        onCheckedChanged: root.autoExtend = checked
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
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            leftPadding: extendBox.indicator.width + 8
                            wrapMode: Text.WordWrap
                        }
                    }
                    Text {
                        //% "Grow the last volume to fill free space on the target"
                        text: qsTrId("aegra.restore.auto_extend_hint")
                        color: Theme.colorTextDim
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Item { Layout.fillHeight: true }

                    AppButton {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        //% "Restore"
                        text: qsTrId("aegra.nav.restore")
                        primary: true
                        enabled: false
                    }
                }

                // Collapsed strip
                Item {
                    anchors.fill: parent
                    visible: root.optionsCollapsed
                    Text {
                        anchors.centerIn: parent
                        rotation: -90
                        //% "Options"
                        text: qsTrId("aegra.restore.options")
                        color: Theme.colorTextWhite
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.optionsCollapsed = false
                    }
                }
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
