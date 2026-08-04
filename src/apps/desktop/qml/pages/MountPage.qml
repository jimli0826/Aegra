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

    property bool optionsCollapsed: true
    property bool checkpointPanelOpen: false
    property real sourceMountedRatio: 0.45
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
                            }
                            Text {
                                //% "Source Disks"
                                text: qsTrId("aegra.mount.source_disks")
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                //% "(check disks to mount — volumes auto-get drive letters)"
                                text: qsTrId("aegra.mount.source_hint")
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
                                wrapMode: Text.WordWrap
                                horizontalAlignment: Text.AlignHCenter
                                //% "Select a checkpoint to view source disks"
                                text: qsTrId("aegra.mount.source_empty")
                                color: Theme.colorTextGrey
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                            }
                        }
                    }
                }

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
                }

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
                        // Column headers
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Rectangle {
                                width: 18
                                height: 18
                                radius: 3
                                color: "transparent"
                                border.width: 1
                                border.color: Theme.colorTextGrey
                            }
                            Text {
                                Layout.preferredWidth: 80
                                //% "Drive(s)"
                                text: qsTrId("aegra.mount.col.drives")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
                            Text {
                                Layout.preferredWidth: 80
                                //% "Disk"
                                text: qsTrId("aegra.mount.col.disk")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
                            Text {
                                Layout.fillWidth: true
                                //% "Image"
                                text: qsTrId("aegra.mount.col.image")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
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
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                }
                                Text {
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    //% "Check source disk(s), then click Mount"
                                    text: qsTrId("aegra.mount.mounted_hint")
                                    color: Theme.colorTextDim
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }
                }
            }

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
                    anchors.margins: 12
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

                    Text {
                        //% "Preferred drive letter"
                        text: qsTrId("aegra.mount.drive_letter")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                    }
                    ComboBox {
                        id: letterCombo
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        model: ["—", "D:", "E:", "F:", "G:", "H:", "I:", "J:", "K:", "L:", "M:",
                                "N:", "O:", "P:", "Q:", "R:", "S:", "T:", "U:", "V:", "W:", "X:",
                                "Y:", "Z:"]
                        enabled: false
                        background: Rectangle {
                            color: Theme.colorInput
                            radius: 4
                            border.width: 1
                            border.color: Theme.colorBorder
                        }
                        contentItem: Text {
                            leftPadding: 10
                            text: letterCombo.displayText
                            color: Theme.colorTextWhite
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    Item { Layout.fillHeight: true }

                    AppButton {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        //% "Mount"
                        text: qsTrId("aegra.nav.mount")
                        primary: true
                        enabled: false
                    }
                    AppButton {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        //% "Unmount"
                        text: qsTrId("aegra.mount.unmount")
                        enabled: false
                    }
                }

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
