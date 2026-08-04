import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Visual baseline: backup/src/gui EventLogPage (filters + table; empty without Service).
Item {
    id: root
    //% "Event Log"
    Accessible.name: qsTrId("aegra.nav.event_log")

    property string timeRange: "all"
    property string typeFilter: "all"
    property string statusFilter: "all"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Row {
                spacing: 8
                Rectangle {
                    width: 3
                    height: 20
                    color: Theme.colorAccentBlue
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    //% "Event Log"
                    text: qsTrId("aegra.nav.event_log")
                    color: Theme.colorTextWhite
                    font.pixelSize: 18
                    font.bold: true
                    font.family: Theme.fontFamily
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            Item { Layout.fillWidth: true }
            AppButton {
                //% "Delete"
                text: qsTrId("aegra.common.delete")
                enabled: false
            }
            AppButton {
                //% "Refresh"
                text: qsTrId("aegra.common.refresh")
                enabled: false
            }
        }

        // Filters row
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            color: Theme.colorCard
            radius: 4
            border.width: 1
            border.color: Theme.colorBorder

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 12

                Text {
                    //% "Time"
                    text: qsTrId("aegra.eventlog.filter.time")
                    color: Theme.colorTextGrey
                    font.pixelSize: 12
                    font.family: Theme.fontFamily
                }
                Repeater {
                    model: [
                        { id: "all", label: qsTrId("aegra.eventlog.range.all") },
                        { id: "24h", label: qsTrId("aegra.eventlog.range.24h") },
                        { id: "7d", label: qsTrId("aegra.eventlog.range.7d") },
                        { id: "30d", label: qsTrId("aegra.eventlog.range.30d") }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        width: Math.max(56, chipLabel.implicitWidth + 20)
                        height: 28
                        radius: 4
                        color: root.timeRange === modelData.id
                               ? Theme.colorAccentBlue
                               : (chipMouse.containsMouse ? Theme.colorButtonHover
                                                          : Theme.colorButton)
                        border.width: root.timeRange === modelData.id ? 0 : 1
                        border.color: Theme.colorBorder
                        Text {
                            id: chipLabel
                            anchors.centerIn: parent
                            text: modelData.label
                            color: Theme.colorTextWhite
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            font.bold: root.timeRange === modelData.id
                        }
                        MouseArea {
                            id: chipMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.timeRange = modelData.id
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Text {
                    //% "Type"
                    text: qsTrId("aegra.home.column.type")
                    color: Theme.colorTextGrey
                    font.pixelSize: 12
                    font.family: Theme.fontFamily
                }
                ComboBox {
                    Layout.preferredWidth: 120
                    Layout.preferredHeight: 28
                    model: [
                        qsTrId("aegra.eventlog.type.all"),
                        qsTrId("aegra.nav.backup"),
                        qsTrId("aegra.nav.restore"),
                        qsTrId("aegra.nav.mount")
                    ]
                    background: Rectangle {
                        color: Theme.colorInput
                        radius: 4
                        border.width: 1
                        border.color: Theme.colorBorder
                    }
                    contentItem: Text {
                        leftPadding: 8
                        text: parent.displayText
                        color: Theme.colorTextWhite
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        verticalAlignment: Text.AlignVCenter
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
                    Layout.preferredWidth: 120
                    Layout.preferredHeight: 28
                    model: [
                        qsTrId("aegra.eventlog.status.all"),
                        qsTrId("aegra.task.state.succeeded"),
                        qsTrId("aegra.task.state.failed"),
                        qsTrId("aegra.task.state.cancelled")
                    ]
                    background: Rectangle {
                        color: Theme.colorInput
                        radius: 4
                        border.width: 1
                        border.color: Theme.colorBorder
                    }
                    contentItem: Text {
                        leftPadding: 8
                        text: parent.displayText
                        color: Theme.colorTextWhite
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }

        // Table
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.colorCard
            radius: 4
            border.width: 1
            border.color: Theme.colorBorder
            clip: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 0
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    color: Theme.colorTableHeader
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 8
                        Text {
                            Layout.preferredWidth: 48
                            //% "Type"
                            text: qsTrId("aegra.home.column.type")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 140
                            Layout.fillWidth: true
                            //% "Summary"
                            text: qsTrId("aegra.eventlog.column.summary")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 90
                            //% "Status"
                            text: qsTrId("aegra.home.column.status")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 150
                            //% "Time"
                            text: qsTrId("aegra.eventlog.filter.time")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 48
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        //% "No events yet. Task history will appear when event log Service APIs are connected."
                        text: qsTrId("aegra.eventlog.empty")
                        color: Theme.colorTextGrey
                        font.pixelSize: 13
                        font.family: Theme.fontFamily
                    }
                }

                // Pagination footer chrome
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    color: Theme.colorTableHeader
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        Text {
                            //% "Page 1 of 1"
                            text: qsTrId("aegra.eventlog.page_of").arg(1).arg(1)
                            color: Theme.colorTextDim
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                        AppButton {
                            //% "Previous"
                            text: qsTrId("aegra.eventlog.prev")
                            enabled: false
                        }
                        AppButton {
                            //% "Next"
                            text: qsTrId("aegra.eventlog.next")
                            enabled: false
                        }
                    }
                }
            }
        }
    }
}
