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

    property bool optionsCollapsed: false
    property bool checkpointPanelOpen: false
    property bool preserveSignature: true
    property bool autoExtend: true
    property real sourceTargetRatio: 0.45
    property real optionsPaneRatio: 0.30

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
            Text {
                //% "Service restore is not connected yet — UI preview only"
                text: qsTrId("aegra.page.preview_only")
                color: Theme.colorTextDim
                font.pixelSize: 11
                font.family: Theme.fontFamily
            }
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
                                //% "Source disks"
                                text: qsTrId("aegra.restore.source_disks")
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                //% "Layout from the selected recovery point"
                                text: qsTrId("aegra.restore.source_hint")
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            AppButton {
                                //% "Select recovery point"
                                text: qsTrId("aegra.restore.select_checkpoint")
                                onClicked: root.checkpointPanelOpen = true
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 32
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                //% "Select a recovery point to show source disk layout"
                                text: qsTrId("aegra.restore.select_checkpoint_source")
                                color: Theme.colorTextGrey
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
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
                                //% "Target disks"
                                text: qsTrId("aegra.restore.target_disks")
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                //% "Map backup disks onto local disks"
                                text: qsTrId("aegra.restore.target_hint")
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Text {
                                anchors.centerIn: parent
                                //% "Local disks will appear when restore inventory is connected"
                                text: qsTrId("aegra.restore.target_empty")
                                color: Theme.colorTextGrey
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                                width: parent.width - 32
                                wrapMode: Text.WordWrap
                                horizontalAlignment: Text.AlignHCenter
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

    // Checkpoint picker drawer (empty UI)
    Item {
        anchors.fill: parent
        z: 2000
        visible: root.checkpointPanelOpen || panel.slideProgress < 0.999

        Rectangle {
            anchors.fill: parent
            color: Theme.colorScrim
            opacity: root.checkpointPanelOpen ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 250 } }
            MouseArea {
                anchors.fill: parent
                enabled: root.checkpointPanelOpen
                onClicked: root.checkpointPanelOpen = false
            }
        }

        Rectangle {
            id: panel
            width: Math.max(420, parent.width * 0.55)
            height: parent.height
            property real slideProgress: root.checkpointPanelOpen ? 0 : 1
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
                    Rectangle {
                        width: 3
                        height: 18
                        color: Theme.colorAccentBlue
                    }
                    Text {
                        //% "Select recovery point"
                        text: qsTrId("aegra.restore.select_checkpoint")
                        color: Theme.colorTextWhite
                        font.pixelSize: 16
                        font.bold: true
                        font.family: Theme.fontFamily
                        Layout.fillWidth: true
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
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: root.checkpointPanelOpen = false
                    }
                }
                Text {
                    Layout.fillWidth: true
                    //% "Recovery point calendar and list will appear when restore Service APIs are connected"
                    text: qsTrId("aegra.restore.checkpoint_panel_empty")
                    color: Theme.colorTextGrey
                    font.pixelSize: 13
                    font.family: Theme.fontFamily
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    Layout.topMargin: 80
                }
                Item { Layout.fillHeight: true }
            }
        }
    }
}
