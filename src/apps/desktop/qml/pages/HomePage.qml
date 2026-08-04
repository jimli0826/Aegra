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

    // Demo disks match old Home when Service inventory is empty.
    readonly property var demoDisks: [
        {
            name: "Disk 0",
            freeFormatted: "19.1 GB",
            size: "20.0 GB",
            mediaType: "SSD",
            isSystemDisk: false,
            percentUsed: 0.045
        },
        {
            name: "Disk 1",
            freeFormatted: "11.5 GB",
            size: "30.0 GB",
            mediaType: "SSD",
            isSystemDisk: true,
            percentUsed: 0.617
        },
        {
            name: "Disk 2",
            freeFormatted: "2.0 GB",
            size: "2.0 GB",
            mediaType: "SSD",
            isSystemDisk: false,
            percentUsed: 0.01
        }
    ]

    // Old Home charts physical disks (GetDisksWithVolumes), not individual volumes.
    readonly property var homeDisks: {
        if (serviceClient.sources && serviceClient.sources.count > 0
                && serviceClient.sources.disksTree
                && serviceClient.sources.disksTree.length > 0)
            return serviceClient.sources.disksTree
        return []
    }

    // Keep demo disks while inventory is empty (including during load) so Home does not
    // flash empty → demo on every connect.
    readonly property bool useDemoDisks: root.homeDisks.length === 0

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

                // Left: system information (old layout)
                ColumnLayout {
                    Layout.fillHeight: true
                    Layout.preferredWidth: 220
                    spacing: 8

                    Text {
                        //% "System Information"
                        text: qsTrId("aegra.home.system_information")
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                    }
                    Text {
                        //% "Windows"
                        text: qsTrId("aegra.home.os_name")
                        color: Theme.colorTextWhite
                        font.pixelSize: 16
                        font.bold: true
                        font.family: Theme.fontFamily
                    }
                    Text {
                        //% "© Microsoft Corporation. All Rights Reserved."
                        text: qsTrId("aegra.home.os_copyright")
                        color: Theme.colorTextGrey
                        font.pixelSize: 11
                        font.family: Theme.fontFamily
                        wrapMode: Text.WordWrap
                        Layout.preferredWidth: 220
                    }
                    Item { Layout.fillHeight: true }
                }

                // Center: physical disks (old homeBackend.disks) or demo charts
                Item {
                    Layout.fillHeight: true
                    Layout.fillWidth: true

                    Flickable {
                        anchors.fill: parent
                        contentWidth: diskRow.implicitWidth
                        contentHeight: height
                        clip: true
                        interactive: diskRow.implicitWidth > width
                        visible: root.homeDisks.length > 0 || root.useDemoDisks
                        boundsBehavior: Flickable.StopAtBounds

                        Row {
                            id: diskRow
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 28

                            // Live: one chart per PhysicalDrive (disksTree), not per volume.
                            Repeater {
                                model: root.useDemoDisks ? root.demoDisks : root.homeDisks
                                delegate: Item {
                                    required property var modelData
                                    width: 110
                                    height: 120
                                    ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 6
                                        DiskChart {
                                            Layout.alignment: Qt.AlignHCenter
                                            width: 72
                                            height: 72
                                            // used/capacity arc; free (approx) in ring center — old Home.
                                            percent: modelData.percentUsed || 0
                                            label: modelData.freeFormatted || modelData.size || ""
                                            diskName: ""
                                            animationEnabled: true
                                        }
                                        Text {
                                            Layout.alignment: Qt.AlignHCenter
                                            Layout.maximumWidth: 108
                                            text: modelData.name || ""
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
                                                // Old Home: size · mediaType · System
                                                var parts = []
                                                if (modelData.size)
                                                    parts.push(modelData.size)
                                                if (modelData.mediaType)
                                                    parts.push(modelData.mediaType)
                                                if (modelData.isSystemDisk)
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
                    //% "Showing latest 10 only"
                    text: qsTrId("aegra.home.tasks_latest_10")
                    color: Theme.colorTextGrey
                    font.pixelSize: 11
                    font.family: Theme.fontFamily
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    //% "More"
                    text: qsTrId("aegra.home.more")
                    color: moreMouse.containsMouse ? "#33b8ff" : Theme.colorAccentBlue
                    font.pixelSize: 12
                    font.bold: true
                    font.family: Theme.fontFamily
                    anchors.verticalCenter: parent.verticalCenter
                    MouseArea {
                        id: moreMouse
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.homeNavigate(5)
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
                                 || (serviceClient.jobs.count === 0
                                     && !serviceClient.jobsLoading)
                        //% "No backup or restore tasks"
                        text: qsTrId("aegra.home.no_backup_restore_tasks")
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
                            required property string stateColor
                            required property int stateValue
                            required property string createdText
                            required property int progressPercent
                            required property bool progressVisible
                            required property bool isActive
                            required property string sourceName
                            required property string destinationName
                            required property string destinationPath
                            required property int index

                            width: taskList.width
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
                                // Source — disk / volume display name (old Home style)
                                Item {
                                    Layout.preferredWidth: 140
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width
                                        text: sourceName
                                        color: Theme.colorTextWhite
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                        elide: Text.ElideMiddle
                                    }
                                }
                                // Destination — repository name + path (two lines when path differs)
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
                                            height: destinationPath.length > 0
                                                    && destinationPath !== destinationName ? 16
                                                                                           : implicitHeight
                                            text: destinationName
                                            color: Theme.colorTextWhite
                                            font.pixelSize: destinationPath.length > 0
                                                            && destinationPath !== destinationName
                                                            ? 11 : 12
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideMiddle
                                        }
                                        Text {
                                            width: parent.width
                                            height: 14
                                            visible: destinationPath.length > 0
                                                     && destinationPath !== destinationName
                                            text: destinationPath
                                            color: Theme.colorTextGrey
                                            font.pixelSize: 9
                                            font.family: Theme.fontFamily
                                            elide: Text.ElideMiddle
                                        }
                                    }
                                }
                                // Progress — bar + percent (old Home style)
                                RowLayout {
                                    Layout.preferredWidth: 140
                                    spacing: 8
                                    Item {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 8
                                        TaskProgressBar {
                                            anchors.fill: parent
                                            value: progressPercent
                                            active: isActive
                                            fillColor: {
                                                // 4=Succeeded 5=Failed 6=Cancelled 7=Interrupted
                                                if (stateValue === 5)
                                                    return Theme.colorAccentRed
                                                if (stateValue === 4)
                                                    return Theme.colorGreen
                                                if (stateValue === 6 || stateValue === 7)
                                                    return "#e6a817"
                                                return Theme.colorAccentBlue
                                            }
                                            visible: progressVisible || isActive || progressPercent > 0
                                        }
                                        Text {
                                            anchors.centerIn: parent
                                            visible: !progressVisible && !isActive && progressPercent === 0
                                            text: "—"
                                            color: Theme.colorTextDim
                                            font.pixelSize: 12
                                            font.family: Theme.fontFamily
                                        }
                                    }
                                    Text {
                                        Layout.preferredWidth: 36
                                        visible: progressVisible || isActive || progressPercent > 0
                                        text: progressPercent + "%"
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 11
                                        font.family: Theme.fontFamily
                                        horizontalAlignment: Text.AlignRight
                                    }
                                }
                                Text {
                                    Layout.preferredWidth: 90
                                    text: stateText
                                    color: stateColor
                                    font.pixelSize: 12
                                    font.bold: true
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
                                Item { Layout.preferredWidth: 72 }
                            }
                        }
                    }
                }
            }
        }
    }
}
