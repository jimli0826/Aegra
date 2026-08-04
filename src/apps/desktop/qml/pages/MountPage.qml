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

    property bool optionsCollapsed: false
    property bool checkpointPanelOpen: false
    property real sourceMountedRatio: 0.45
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
                                //% "Source disks"
                                text: qsTrId("aegra.mount.source_disks")
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                //% "Disk layout inside the selected recovery point"
                                text: qsTrId("aegra.mount.source_hint")
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
                                wrapMode: Text.WordWrap
                                horizontalAlignment: Text.AlignHCenter
                                //% "Select a recovery point to show mountable disks"
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
                                //% "Mounted sessions"
                                text: qsTrId("aegra.mount.mounted")
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Text {
                                //% "Active mount sessions on this machine"
                                text: qsTrId("aegra.mount.mounted_hint")
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
                                //% "No mounted sessions"
                                text: qsTrId("aegra.mount.mounted_empty")
                                color: Theme.colorTextGrey
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
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

    Item {
        anchors.fill: parent
        z: 2000
        visible: root.checkpointPanelOpen || mp.slideProgress < 0.999
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
            id: mp
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
                    Layout.topMargin: 80
                    //% "Recovery point list will appear when mount Service APIs are connected"
                    text: qsTrId("aegra.mount.checkpoint_panel_empty")
                    color: Theme.colorTextGrey
                    font.pixelSize: 13
                    font.family: Theme.fontFamily
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }
                Item { Layout.fillHeight: true }
            }
        }
    }
}
