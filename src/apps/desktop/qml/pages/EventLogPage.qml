import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Task Log: terminal (completed) jobs from ListJobs scope=terminal.
Item {
    id: root
    //% "Task Log"
    Accessible.name: qsTrId("aegra.nav.event_log")

    property int timeIndex: 0
    property int typeIndex: 0
    property int statusIndex: 0

    readonly property var logModel: (typeof serviceClient !== "undefined" && serviceClient)
                                    ? serviceClient.taskLog : null
    readonly property bool logLoading: (typeof serviceClient !== "undefined" && serviceClient)
                                       ? serviceClient.taskLogLoading : false
    readonly property bool logHasMore: (typeof serviceClient !== "undefined" && serviceClient)
                                      ? serviceClient.taskLogHasMore : false
    readonly property string logError: (typeof serviceClient !== "undefined" && serviceClient)
                                       ? (serviceClient.taskLogErrorText || "") : ""
    readonly property int logCount: logModel ? logModel.count : 0

    function reload() {
        if (typeof serviceClient === "undefined" || !serviceClient || !serviceClient.jobListAvailable)
            return
        serviceClient.refreshTaskLog(root.timeIndex, root.typeIndex, root.statusIndex)
    }

    function loadMore() {
        if (typeof serviceClient === "undefined" || !serviceClient)
            return
        serviceClient.loadMoreTaskLog()
    }

    Component.onCompleted: reload()

    onTimeIndexChanged: reload()
    onTypeIndexChanged: reload()
    onStatusIndexChanged: reload()

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
                    //% "Task Log"
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
                //% "Refresh"
                text: qsTrId("aegra.common.refresh")
                enabled: !root.logLoading
                onClicked: root.reload()
            }
        }

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
                        qsTrId("aegra.job.operation.verify")
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
                            Layout.preferredWidth: 80
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
                            Layout.preferredWidth: 100
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

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ListView {
                        id: eventList
                        anchors.fill: parent
                        clip: true
                        model: root.logModel
                        visible: root.logCount > 0
                        delegate: Rectangle {
                            width: eventList.width
                            height: 48
                            color: index % 2 === 0 ? Theme.colorTableRow : Theme.colorTableAlt

                            required property int index
                            required property string operationText
                            required property string sourceName
                            required property string destinationName
                            required property string destinationPath
                            required property string stateText
                            required property color stateColor
                            required property string createdText

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 8

                                Text {
                                    Layout.preferredWidth: 80
                                    text: operationText || ""
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.preferredWidth: 140
                                    Layout.fillWidth: true
                                    text: sourceName || ""
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
                                            text: destinationName || ""
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 11
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideMiddle
                                        }
                                        Text {
                                            width: parent.width
                                            text: destinationPath || ""
                                            color: Theme.colorTextGrey
                                            font.pixelSize: 9
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideMiddle
                                            visible: (destinationPath || "").length > 0
                                        }
                                    }
                                }
                                Text {
                                    Layout.preferredWidth: 100
                                    text: stateText || ""
                                    color: stateColor
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    font.bold: true
                                }
                                Text {
                                    Layout.preferredWidth: 150
                                    text: createdText || ""
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 11
                                    font.family: Theme.fontFamily
                                }
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: !root.logLoading && root.logCount === 0
                        //% "No events"
                        text: root.logError.length > 0 ? root.logError
                                                       : qsTrId("aegra.eventlog.empty")
                        color: Theme.colorTextDim
                        font.pixelSize: 13
                        font.family: Theme.fontFamily
                    }

                    BusyIndicator {
                        anchors.centerIn: parent
                        running: root.logLoading && root.logCount === 0
                        visible: running
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    color: "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        Text {
                            //% "%1 items"
                            text: qsTrId("aegra.eventlog.page_range")
                                  .arg(root.logCount > 0 ? 1 : 0)
                                  .arg(root.logCount)
                                  .arg(root.logCount)
                                  .arg(1)
                                  .arg(1)
                            color: Theme.colorTextDim
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                        }
                        Item { Layout.fillWidth: true }
                        AppButton {
                            //% "Load more"
                            text: qsTrId("aegra.eventlog.next")
                            enabled: root.logHasMore && !root.logLoading
                            visible: root.logHasMore
                            onClicked: root.loadMore()
                        }
                    }
                }
            }
        }
    }
}
