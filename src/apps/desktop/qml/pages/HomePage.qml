import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Visual baseline: backup/src/gui HomePage — This PC (1/3) + Tasks table (2/3).
Item {
    id: root
    //% "Home"
    Accessible.name: qsTrId("aegra.nav.home")
    signal homeNavigate(int index)

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        // Top: This PC — ~1/3
        Card {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: 1
            //% "This PC"
            title: qsTrId("aegra.home.this_pc")

            RowLayout {
                anchors.fill: parent
                anchors.topMargin: 50
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                anchors.bottomMargin: 16
                spacing: 30

                // Left: service / system summary
                ColumnLayout {
                    Layout.fillHeight: true
                    Layout.preferredWidth: 220
                    spacing: 8

                    Text {
                        //% "System information"
                        text: qsTrId("aegra.home.system_information")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                    }
                    Text {
                        //% "Service"
                        text: qsTrId("aegra.home.card.service")
                        color: Theme.colorTextWhite
                        font.pixelSize: 16
                        font.bold: true
                        font.family: Theme.fontFamily
                    }
                    Text {
                        text: serviceClient.statusText
                              + (serviceClient.serviceVersion.length > 0
                                 ? (" · V" + serviceClient.serviceVersion) : "")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                        Layout.preferredWidth: 220
                    }
                    Text {
                        //% "Repository"
                        text: qsTrId("aegra.home.card.repository")
                        color: Theme.colorTextWhite
                        font.pixelSize: 13
                        font.bold: true
                        font.family: Theme.fontFamily
                        Layout.topMargin: 8
                    }
                    Text {
                        text: serviceClient.repositoryStatusText
                        color: Theme.colorTextGrey
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                        Layout.preferredWidth: 220
                    }
                    Item { Layout.fillHeight: true }
                }

                // Center: inventory sources as disk charts
                Item {
                    Layout.fillHeight: true
                    Layout.fillWidth: true

                    Text {
                        anchors.centerIn: parent
                        visible: serviceClient.inventoryLoading
                                 && serviceClient.sources.count === 0
                        //% "Loading sources…"
                        text: qsTrId("aegra.home.loading_disks")
                        color: Theme.colorTextGrey
                        font.pixelSize: 13
                        font.family: Theme.fontFamily
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: !serviceClient.inventoryAvailable
                                 && !serviceClient.inventoryLoading
                        //% "Source inventory unavailable"
                        text: qsTrId("aegra.home.disks_unavailable")
                        color: Theme.colorTextGrey
                        font.pixelSize: 13
                        font.family: Theme.fontFamily
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: serviceClient.inventoryAvailable
                                 && !serviceClient.inventoryLoading
                                 && serviceClient.sources.count === 0
                        //% "No disks found"
                        text: qsTrId("aegra.home.no_disks")
                        color: Theme.colorTextGrey
                        font.pixelSize: 13
                        font.family: Theme.fontFamily
                    }

                    Flickable {
                        anchors.fill: parent
                        contentWidth: diskRow.implicitWidth
                        contentHeight: height
                        clip: true
                        interactive: diskRow.implicitWidth > width
                        visible: serviceClient.sources.count > 0
                        boundsBehavior: Flickable.StopAtBounds

                        Row {
                            id: diskRow
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 28

                            Repeater {
                                model: serviceClient.sources
                                delegate: Item {
                                    required property string displayName
                                    required property string capacityText
                                    required property bool isSystem
                                    required property bool isSelectable
                                    width: 110
                                    height: 120

                                    ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 6
                                        DiskChart {
                                            Layout.alignment: Qt.AlignHCenter
                                            width: 72
                                            height: 72
                                            percent: isSelectable ? 0.35 : 0
                                            label: capacityText
                                            diskName: ""
                                            animationEnabled: true
                                        }
                                        Text {
                                            Layout.alignment: Qt.AlignHCenter
                                            Layout.maximumWidth: 108
                                            text: displayName
                                            color: Theme.colorTextWhite
                                            font.pixelSize: 12
                                            font.bold: true
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideRight
                                            horizontalAlignment: Text.AlignHCenter
                                        }
                                        Text {
                                            Layout.alignment: Qt.AlignHCenter
                                            Layout.maximumWidth: 108
                                            text: {
                                                var parts = []
                                                if (capacityText.length > 0)
                                                    parts.push(capacityText)
                                                if (isSystem)
                                                    parts.push(qsTrId("aegra.home.system_tag"))
                                                return parts.join(" · ")
                                            }
                                            color: Theme.colorTextGrey
                                            font.pixelSize: 10
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideRight
                                            horizontalAlignment: Text.AlignHCenter
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Right: Windows-style tiles (decorative, matches old Home)
                Item {
                    Layout.preferredWidth: 90
                    Layout.fillHeight: true
                    Grid {
                        anchors.centerIn: parent
                        columns: 2
                        spacing: 4
                        Rectangle { width: 32; height: 32; color: Theme.colorTextWhite }
                        Rectangle { width: 32; height: 32; color: Theme.colorTextWhite }
                        Rectangle { width: 32; height: 32; color: Theme.colorTextWhite }
                        Rectangle { width: 32; height: 32; color: Theme.colorTextWhite }
                    }
                }
            }
        }

        // Bottom: Tasks — ~2/3
        Card {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: 2
            //% "Tasks"
            title: qsTrId("aegra.home.card.tasks")

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.top: parent.top
                anchors.topMargin: 11
                spacing: 12
                z: 2

                Text {
                    //% "Latest tasks from Service"
                    text: qsTrId("aegra.home.tasks_latest_hint")
                    color: Theme.colorTextGrey
                    font.pixelSize: 11
                    font.family: Theme.fontFamily
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    //% "Refresh"
                    text: qsTrId("aegra.common.refresh")
                    color: refreshMouse.containsMouse ? "#33b8ff" : Theme.colorAccentBlue
                    font.pixelSize: 12
                    font.bold: true
                    font.family: Theme.fontFamily
                    anchors.verticalCenter: parent.verticalCenter
                    opacity: serviceClient.jobListAvailable ? 1 : 0.45
                    MouseArea {
                        id: refreshMouse
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        enabled: serviceClient.connected && serviceClient.jobListAvailable
                                 && !serviceClient.jobsLoading
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: serviceClient.refreshJobs()
                    }
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 44
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                anchors.bottomMargin: 12
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    color: Theme.colorTableHeader
                    radius: 2

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
                            //% "Job"
                            text: qsTrId("aegra.home.column.job")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 140
                            //% "Progress"
                            text: qsTrId("aegra.home.column.progress")
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
                            Layout.preferredWidth: 130
                            //% "Started"
                            text: qsTrId("aegra.home.column.started")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Item { Layout.preferredWidth: 72 }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Text {
                        anchors.centerIn: parent
                        visible: !serviceClient.jobListAvailable
                        //% "Service does not expose job.list yet"
                        text: qsTrId("aegra.home.jobs_capability_missing")
                        color: Theme.colorTextGrey
                        font.pixelSize: 13
                        font.family: Theme.fontFamily
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: serviceClient.jobListAvailable
                                 && serviceClient.jobs.count === 0
                                 && !serviceClient.jobsLoading
                        //% "No tasks yet"
                        text: qsTrId("aegra.home.jobs_empty")
                        color: Theme.colorTextGrey
                        font.pixelSize: 13
                        font.family: Theme.fontFamily
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: serviceClient.jobsErrorText.length > 0
                        text: serviceClient.jobsErrorText
                        color: Theme.colorAccentRed
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        width: parent.width - 40
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                    }

                    ListView {
                        id: taskList
                        anchors.fill: parent
                        clip: true
                        spacing: 0
                        visible: serviceClient.jobListAvailable && serviceClient.jobs.count > 0
                        model: serviceClient.jobs
                        boundsBehavior: Flickable.StopAtBounds

                        delegate: Rectangle {
                            required property string jobId
                            required property string operationText
                            required property string stateText
                            required property string createdText
                            required property int progressPercent
                            required property bool progressVisible
                            required property string messageText
                            required property bool isActive
                            required property int index

                            width: taskList.width
                            height: 44
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
                                Column {
                                    Layout.preferredWidth: 140
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Text {
                                        width: parent.width
                                        text: operationText
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        width: parent.width
                                        text: jobId
                                        color: Theme.colorTextDim
                                        font.pixelSize: 10
                                        font.family: Theme.fontFamily
                                        elide: Text.ElideMiddle
                                    }
                                }
                                Item {
                                    Layout.preferredWidth: 140
                                    Layout.fillHeight: true
                                    TaskProgressBar {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width
                                        value: progressPercent
                                        active: isActive && progressVisible
                                        visible: progressVisible || isActive
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        visible: !progressVisible && !isActive
                                        text: "—"
                                        color: Theme.colorTextDim
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                    }
                                }
                                Text {
                                    Layout.preferredWidth: 90
                                    text: stateText
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.preferredWidth: 130
                                    text: createdText
                                    color: Theme.colorTextGrey
                                    font.pixelSize: 11
                                    font.family: Theme.fontFamily
                                    elide: Text.ElideRight
                                }
                                Item {
                                    Layout.preferredWidth: 72
                                    Layout.fillHeight: true
                                    Text {
                                        anchors.centerIn: parent
                                        //% "Backup"
                                        text: qsTrId("aegra.nav.backup")
                                        color: goMouse.containsMouse ? "#33b8ff"
                                                                     : Theme.colorAccentBlue
                                        font.pixelSize: 11
                                        font.bold: true
                                        font.family: Theme.fontFamily
                                        MouseArea {
                                            id: goMouse
                                            anchors.fill: parent
                                            anchors.margins: -4
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.homeNavigate(1)
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
