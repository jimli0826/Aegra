import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Visual baseline: backup/src/gui EventLogPage (filters + table + pagination).
Item {
    id: root
    //% "Event Log"
    Accessible.name: qsTrId("aegra.nav.event_log")

    property int timeIndex: 0
    property int typeIndex: 0
    property int statusIndex: 0
    property int page: 1

    // Demo row matching old Event Log screenshot until Service history API is wired.
    readonly property var demoEvents: [
        {
            typeKey: "backup",
            sourceName: "disk0",
            destName: "qqqq",
            destPath: "E:\\qqqq",
            statusKey: "success",
            statusText: qsTrId("aegra.eventlog.status.success"),
            started: "2026-08-04 11:18:47"
        }
    ]

    function themedComboBackground(combo) {
        return null
    }

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
                enabled: true
            }
        }

        // Filters row — ComboBoxes like old UI
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
                ComboBox {
                    id: timeCombo
                    Layout.preferredWidth: 120
                    Layout.preferredHeight: 28
                    model: [
                        qsTrId("aegra.eventlog.range.all"),
                        qsTrId("aegra.eventlog.range.24h"),
                        qsTrId("aegra.eventlog.range.7d"),
                        qsTrId("aegra.eventlog.range.30d")
                    ]
                    currentIndex: root.timeIndex
                    onActivated: root.timeIndex = currentIndex
                    background: Rectangle {
                        color: Theme.colorInput
                        radius: 4
                        border.width: 1
                        border.color: Theme.colorBorder
                    }
                    contentItem: Text {
                        leftPadding: 8
                        rightPadding: 22
                        text: timeCombo.displayText
                        color: Theme.colorTextWhite
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        verticalAlignment: Text.AlignVCenter
                    }
                    indicator: ComboBoxIndicator { combo: timeCombo }
                    popup: Popup {
                        y: timeCombo.height + 2
                        width: timeCombo.width
                        padding: 2
                        implicitHeight: Math.min(180, contentItem.implicitHeight + 4)
                        contentItem: ListView {
                            clip: true
                            implicitHeight: contentHeight
                            model: timeCombo.popup.visible ? timeCombo.delegateModel : null
                            currentIndex: timeCombo.highlightedIndex
                        }
                        background: Rectangle {
                            color: Theme.colorPopup
                            border.color: Theme.colorBorder
                            radius: 4
                        }
                    }
                    delegate: ItemDelegate {
                        width: timeCombo.width
                        height: 28
                        contentItem: Text {
                            text: modelData
                            color: Theme.colorTextWhite
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 8
                        }
                        background: Rectangle {
                            color: parent.highlighted ? Theme.colorAccentBlue : "transparent"
                        }
                    }
                }

                Text {
                    //% "Type"
                    text: qsTrId("aegra.home.column.type")
                    color: Theme.colorTextGrey
                    font.pixelSize: 12
                    font.family: Theme.fontFamily
                }
                ComboBox {
                    id: typeCombo
                    Layout.preferredWidth: 100
                    Layout.preferredHeight: 28
                    model: [
                        qsTrId("aegra.eventlog.type.all"),
                        qsTrId("aegra.nav.backup"),
                        qsTrId("aegra.nav.restore"),
                        qsTrId("aegra.nav.mount")
                    ]
                    currentIndex: root.typeIndex
                    onActivated: root.typeIndex = currentIndex
                    background: Rectangle {
                        color: Theme.colorInput
                        radius: 4
                        border.width: 1
                        border.color: Theme.colorBorder
                    }
                    contentItem: Text {
                        leftPadding: 8
                        rightPadding: 22
                        text: typeCombo.displayText
                        color: Theme.colorTextWhite
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        verticalAlignment: Text.AlignVCenter
                    }
                    indicator: ComboBoxIndicator { combo: typeCombo }
                    popup: Popup {
                        y: typeCombo.height + 2
                        width: typeCombo.width
                        padding: 2
                        implicitHeight: Math.min(160, contentItem.implicitHeight + 4)
                        contentItem: ListView {
                            clip: true
                            implicitHeight: contentHeight
                            model: typeCombo.popup.visible ? typeCombo.delegateModel : null
                            currentIndex: typeCombo.highlightedIndex
                        }
                        background: Rectangle {
                            color: Theme.colorPopup
                            border.color: Theme.colorBorder
                            radius: 4
                        }
                    }
                    delegate: ItemDelegate {
                        width: typeCombo.width
                        height: 28
                        contentItem: Text {
                            text: modelData
                            color: Theme.colorTextWhite
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 8
                        }
                        background: Rectangle {
                            color: parent.highlighted ? Theme.colorAccentBlue : "transparent"
                        }
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
                    id: statusCombo
                    Layout.preferredWidth: 100
                    Layout.preferredHeight: 28
                    model: [
                        qsTrId("aegra.eventlog.status.all"),
                        qsTrId("aegra.eventlog.status.success"),
                        qsTrId("aegra.task.state.failed"),
                        qsTrId("aegra.task.state.cancelled")
                    ]
                    currentIndex: root.statusIndex
                    onActivated: root.statusIndex = currentIndex
                    background: Rectangle {
                        color: Theme.colorInput
                        radius: 4
                        border.width: 1
                        border.color: Theme.colorBorder
                    }
                    contentItem: Text {
                        leftPadding: 8
                        rightPadding: 22
                        text: statusCombo.displayText
                        color: Theme.colorTextWhite
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        verticalAlignment: Text.AlignVCenter
                    }
                    indicator: ComboBoxIndicator { combo: statusCombo }
                    popup: Popup {
                        y: statusCombo.height + 2
                        width: statusCombo.width
                        padding: 2
                        implicitHeight: Math.min(160, contentItem.implicitHeight + 4)
                        contentItem: ListView {
                            clip: true
                            implicitHeight: contentHeight
                            model: statusCombo.popup.visible ? statusCombo.delegateModel : null
                            currentIndex: statusCombo.highlightedIndex
                        }
                        background: Rectangle {
                            color: Theme.colorPopup
                            border.color: Theme.colorBorder
                            radius: 4
                        }
                    }
                    delegate: ItemDelegate {
                        width: statusCombo.width
                        height: 28
                        contentItem: Text {
                            text: modelData
                            color: Theme.colorTextWhite
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 8
                        }
                        background: Rectangle {
                            color: parent.highlighted ? Theme.colorAccentBlue : "transparent"
                        }
                    }
                }

                Item { Layout.fillWidth: true }
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
                            //% "Source"
                            text: qsTrId("aegra.backup.section.source")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 180
                            Layout.fillWidth: true
                            //% "Destination"
                            text: qsTrId("aegra.backup.column.destination")
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
                            //% "Started"
                            text: qsTrId("aegra.home.column.started")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                    }
                }

                ListView {
                    id: eventList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.demoEvents
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: eventList.width
                        height: 48
                        color: index % 2 === 0 ? Theme.colorTableRow : Theme.colorTableAlt

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            Item {
                                Layout.preferredWidth: 48
                                Layout.fillHeight: true
                                Text {
                                    anchors.centerIn: parent
                                    text: "\uEA35"
                                    font.pixelSize: 16
                                    font.family: "Segoe MDL2 Assets"
                                    color: Theme.colorAccentBlue
                                }
                            }
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
                                Layout.preferredWidth: 180
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Column {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    spacing: 0
                                    Text {
                                        width: parent.width
                                        text: modelData.destName || ""
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 11
                                        font.family: Theme.fontFamily
                                        elide: Text.ElideMiddle
                                    }
                                    Text {
                                        width: parent.width
                                        text: modelData.destPath || ""
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 9
                                        font.family: Theme.fontFamily
                                        elide: Text.ElideMiddle
                                        visible: (modelData.destPath || "").length > 0
                                    }
                                }
                            }
                            Text {
                                Layout.preferredWidth: 90
                                text: modelData.statusText || ""
                                color: modelData.statusKey === "success"
                                       ? Theme.colorGreen : Theme.colorTextWhite
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                font.bold: modelData.statusKey === "success"
                            }
                            Text {
                                Layout.preferredWidth: 150
                                text: modelData.started || ""
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                            }
                        }
                    }
                }

                // Pagination footer — old style
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    color: "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        Text {
                            //% "%1–%2 of %3 · Page %4 / %5"
                            text: qsTrId("aegra.eventlog.page_range")
                                  .arg(1).arg(root.demoEvents.length)
                                  .arg(root.demoEvents.length).arg(1).arg(1)
                            color: Theme.colorTextDim
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            width: 32
                            height: 28
                            radius: 4
                            color: Theme.colorButton
                            border.width: 1
                            border.color: Theme.colorBorder
                            opacity: 0.5
                            Text {
                                anchors.centerIn: parent
                                text: "\u2039"
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                            }
                        }
                        Rectangle {
                            width: 32
                            height: 28
                            radius: 4
                            color: Theme.colorButton
                            border.width: 1
                            border.color: Theme.colorBorder
                            opacity: 0.5
                            Text {
                                anchors.centerIn: parent
                                text: "\u203A"
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                            }
                        }
                    }
                }
            }
        }
    }
}
