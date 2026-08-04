import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Visual baseline: backup/src/gui BackupPage — schedule list + right slide-in wizard.
// Data: ServiceClient inventory / connections / startBackup / cancel (no old Backend).
Item {
    id: root
    //% "Backup"
    Accessible.name: qsTrId("aegra.nav.backup")

    property bool wizardOpen: false
    property string selectedSourceId: ""
    property string selectedConnectionId: ""
    property bool confirmChecked: false

    readonly property bool fullBackupReady: serviceClient.connected
                                            && serviceClient.backupStartAvailable
                                            && selectedSourceId.length > 0
                                            && selectedConnectionId.length > 0
                                            && confirmChecked
                                            && !serviceClient.backupCommandBusy
                                            && !serviceClient.cancelCommandBusy
                                            && (serviceClient.activeBackupJobId.length === 0
                                                || serviceClient.activeBackupTerminal)

    function openWizard() {
        root.confirmChecked = false
        if (root.selectedConnectionId.length === 0)
            root.selectedConnectionId = serviceClient.defaultConnectionId()
        root.wizardOpen = true
        serviceClient.refreshInventory()
        serviceClient.refreshConnections()
    }

    function closeWizard() {
        root.wizardOpen = false
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        // Page title — old GUI: 3px accent + 18px title
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Row {
                spacing: 8
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    width: 3
                    height: 20
                    color: Theme.colorAccentBlue
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    //% "Backup"
                    text: qsTrId("aegra.nav.backup")
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
                //% "Reconnect"
                text: serviceClient.connected
                      ? qsTrId("aegra.common.refresh")
                      : qsTrId("aegra.common.reconnect")
                enabled: !serviceClient.inventoryLoading && !serviceClient.connectionsLoading
                onClicked: {
                    if (serviceClient.connected) {
                        serviceClient.refreshInventory()
                        serviceClient.refreshConnections()
                        serviceClient.refreshJobs()
                    } else {
                        serviceClient.reconnect()
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: !serviceClient.connected || !serviceClient.backupStartAvailable
            //% "Backup requires Service capabilities: source.inventory, repository.connection, backup.start"
            text: qsTrId("aegra.backup.capability.missing")
            color: Theme.colorAccentRed
            font.pixelSize: 12
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
        }

        // Main list card — schedules table (S8 disabled empty state)
        Card {
            Layout.fillWidth: true
            Layout.fillHeight: true
            //% "Schedules"
            title: qsTrId("aegra.backup.section.schedule")

            AppButton {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.top: parent.top
                anchors.topMargin: 8
                z: 2
                //% "Add"
                text: qsTrId("aegra.common.add")
                enabled: serviceClient.connected && serviceClient.backupStartAvailable
                onClicked: root.openWizard()
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 44
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                anchors.bottomMargin: 12
                spacing: 0

                // Table header
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
                            Layout.preferredWidth: 140
                            Layout.fillWidth: true
                            //% "Source"
                            text: qsTrId("aegra.backup.section.source")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.preferredWidth: 100
                            Layout.fillWidth: true
                            //% "Target repository"
                            text: qsTrId("aegra.backup.section.target")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.preferredWidth: 110
                            //% "Type"
                            text: qsTrId("aegra.backup.type.full")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Text {
                            Layout.preferredWidth: 130
                            //% "Status"
                            text: qsTrId("aegra.backup.column.status")
                            color: Theme.colorTextGrey
                            font.pixelSize: 12
                            font.bold: true
                            font.family: Theme.fontFamily
                        }
                        Item { Layout.preferredWidth: 100 }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    // Empty schedules (engine not wired)
                    Column {
                        anchors.centerIn: parent
                        width: parent.width - 48
                        spacing: 10
                        visible: serviceClient.activeBackupJobId.length === 0
                                 && !serviceClient.backupCommandBusy

                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            //% "No schedules yet"
                            text: qsTrId("aegra.backup.schedules.empty")
                            color: Theme.colorTextGrey
                            font.pixelSize: 13
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            //% "Scheduling is not available yet"
                            text: qsTrId("aegra.backup.schedule.unavailable")
                            color: Theme.colorTextDim
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            //% "Use Add to start a one-time full backup"
                            text: qsTrId("aegra.backup.schedules.hint_add")
                            color: Theme.colorTextDim
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                        }
                    }

                    // Active backup row (job observation, same table row chrome)
                    Rectangle {
                        anchors.top: parent.top
                        width: parent.width
                        height: 52
                        visible: serviceClient.activeBackupJobId.length > 0
                                 || serviceClient.backupCommandBusy
                        color: Theme.colorTableRow

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            Text {
                                Layout.preferredWidth: 140
                                Layout.fillWidth: true
                                text: root.selectedSourceId.length > 0
                                      ? root.selectedSourceId
                                      : "—"
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                elide: Text.ElideMiddle
                            }
                            Text {
                                Layout.preferredWidth: 100
                                Layout.fillWidth: true
                                text: root.selectedConnectionId.length > 0
                                      ? root.selectedConnectionId
                                      : "—"
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                elide: Text.ElideMiddle
                            }
                            Text {
                                Layout.preferredWidth: 110
                                //% "Full"
                                text: qsTrId("aegra.backup.type.full")
                                color: Theme.colorTextWhite
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                            }
                            ColumnLayout {
                                Layout.preferredWidth: 160
                                spacing: 2
                                Text {
                                    text: serviceClient.activeBackupStateText
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                }
                                TaskProgressBar {
                                    Layout.fillWidth: true
                                    visible: serviceClient.activeBackupProgressVisible
                                             || serviceClient.backupCommandBusy
                                    value: serviceClient.activeBackupProgressPercent
                                    active: !serviceClient.activeBackupTerminal
                                }
                            }
                            AppButton {
                                //% "Cancel job"
                                text: qsTrId("aegra.backup.action.cancel")
                                danger: true
                                enabled: serviceClient.activeBackupCancellable
                                         && !serviceClient.cancelCommandBusy
                                onClicked: serviceClient.cancelActiveBackup()
                            }
                            AppButton {
                                //% "Details"
                                text: qsTrId("aegra.backup.action.details")
                                onClicked: root.openWizard()
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Right slide-in wizard (90% width) — same pattern as old BackupPage ──
    Item {
        id: wizardDrawer
        anchors.fill: parent
        z: 2000

        Rectangle {
            id: wizardScrim
            anchors.fill: parent
            color: Theme.colorScrim
            opacity: root.wizardOpen ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 250 } }
            MouseArea {
                anchors.fill: parent
                enabled: root.wizardOpen
            }
        }

        Rectangle {
            id: wizardPanel
            width: Math.max(420, parent.width * 9 / 10)
            height: parent.height
            y: 0
            property real slideProgress: root.wizardOpen ? 0 : 1
            x: parent.width - width + slideProgress * width
            Behavior on slideProgress {
                NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
            }
            color: Theme.colorBg
            border.width: 1
            border.color: Theme.colorBorder

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: Theme.colorBorder
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                // Wizard header
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    spacing: 8

                    Rectangle {
                        width: 3
                        height: 18
                        color: Theme.colorAccentBlue
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        //% "Start backup"
                        text: qsTrId("aegra.backup.wizard.title")
                        color: Theme.colorTextWhite
                        font.pixelSize: 16
                        font.bold: true
                        font.family: Theme.fontFamily
                        Layout.alignment: Qt.AlignVCenter
                        Layout.fillWidth: true
                        elide: Text.ElideRight
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
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: root.closeWizard()
                    }
                }

                // Source + destination row
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 20

                    // SOURCE
                    Card {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        //% "SOURCE"
                        title: qsTrId("aegra.backup.section.source_upper")

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.topMargin: 44
                            anchors.margins: 12
                            spacing: 8

                            Text {
                                visible: serviceClient.inventoryErrorText.length > 0
                                text: serviceClient.inventoryErrorText
                                color: Theme.colorAccentRed
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            BusyIndicator {
                                running: serviceClient.inventoryLoading
                                visible: running
                                Layout.preferredWidth: 22
                                Layout.preferredHeight: 22
                                Layout.alignment: Qt.AlignHCenter
                            }

                            Text {
                                visible: serviceClient.inventoryAvailable
                                         && !serviceClient.inventoryLoading
                                         && serviceClient.sources.count === 0
                                         && serviceClient.inventoryErrorText.length === 0
                                //% "No backup sources reported by Service"
                                text: qsTrId("aegra.backup.source.empty")
                                color: Theme.colorTextGrey
                                font.pixelSize: 12
                                font.family: Theme.fontFamily
                                Layout.alignment: Qt.AlignHCenter
                            }

                            ListView {
                                id: sourceList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 6
                                model: serviceClient.sources
                                boundsBehavior: Flickable.StopAtBounds
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                                delegate: Rectangle {
                                    id: sourceRow
                                    required property string sourceId
                                    required property string displayName
                                    required property string capacityText
                                    required property string availabilityText
                                    required property bool isSelectable
                                    required property string disabledReasonText
                                    required property bool isSystem
                                    required property bool isReadOnly

                                    width: sourceList.width
                                    height: 45
                                    radius: 4
                                    color: sourceHover.containsMouse && isSelectable
                                           ? Theme.colorHover : Theme.colorListItem
                                    opacity: isSelectable ? 1.0 : 0.55
                                    Accessible.name: displayName + " " + capacityText

                                    readonly property bool selected:
                                        root.selectedSourceId === sourceId

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        spacing: 10

                                        // Checkbox
                                        Rectangle {
                                            width: 18
                                            height: 18
                                            visible: isSelectable
                                            radius: 3
                                            color: sourceRow.selected
                                                   ? Theme.colorAccentBlue : "transparent"
                                            border.width: 2
                                            border.color: sourceRow.selected
                                                          ? Theme.colorAccentBlue
                                                          : Theme.colorTextGrey
                                            Text {
                                                anchors.centerIn: parent
                                                text: sourceRow.selected ? "\u2713" : ""
                                                color: "white"
                                                font.pixelSize: 12
                                                font.bold: true
                                            }
                                        }
                                        Item {
                                            width: 18
                                            height: 18
                                            visible: !isSelectable
                                        }

                                        DiskIcon {
                                            size: 28
                                            iconOpacity: isSelectable ? 1.0 : 0.55
                                            variant: isSystem ? "system" : "hdd"
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Text {
                                                text: displayName + (capacityText.length > 0
                                                      ? (" (" + capacityText + ")") : "")
                                                color: Theme.colorTextWhite
                                                font.pixelSize: 13
                                                font.bold: true
                                                font.family: Theme.fontFamily
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Text {
                                                text: isSelectable ? availabilityText
                                                                   : disabledReasonText
                                                color: isSelectable ? Theme.colorTextGrey
                                                                    : Theme.colorAccentRed
                                                font.pixelSize: 11
                                                font.family: Theme.fontFamily
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                        }
                                    }

                                    MouseArea {
                                        id: sourceHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        enabled: isSelectable
                                        cursorShape: isSelectable ? Qt.PointingHandCursor
                                                                  : Qt.ArrowCursor
                                        onClicked: {
                                            root.selectedSourceId = sourceId
                                            root.confirmChecked = false
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // DESTINATION
                    Card {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        //% "DESTINATION"
                        title: qsTrId("aegra.backup.section.destination_upper")

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.topMargin: 44
                            anchors.margins: 12
                            spacing: 8

                            Text {
                                //% "Repository"
                                text: qsTrId("aegra.nav.repository")
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                color: Theme.colorBg
                                radius: 4
                                border.width: 1
                                border.color: Theme.colorBorder

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    spacing: 2

                                    Text {
                                        visible: serviceClient.connectionsErrorText.length > 0
                                        text: serviceClient.connectionsErrorText
                                        color: Theme.colorAccentRed
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                        Layout.margins: 8
                                    }

                                    BusyIndicator {
                                        running: serviceClient.connectionsLoading
                                        visible: running
                                        Layout.preferredWidth: 22
                                        Layout.preferredHeight: 22
                                        Layout.alignment: Qt.AlignHCenter
                                    }

                                    Text {
                                        visible: serviceClient.connectionsAvailable
                                                 && !serviceClient.connectionsLoading
                                                 && serviceClient.connections.count === 0
                                        //% "No repository connection configured"
                                        text: qsTrId("aegra.backup.target.empty")
                                        color: Theme.colorTextGrey
                                        font.pixelSize: 12
                                        font.family: Theme.fontFamily
                                        Layout.alignment: Qt.AlignHCenter
                                        Layout.topMargin: 40
                                    }

                                    ListView {
                                        id: connectionList
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        clip: true
                                        spacing: 2
                                        model: serviceClient.connections

                                        delegate: Rectangle {
                                            id: connRow
                                            required property string connectionId
                                            required property string displayName
                                            required property string stateText
                                            required property bool isAvailable
                                            required property bool isDefault

                                            width: connectionList.width
                                            height: 46
                                            radius: 4
                                            color: locHover.containsMouse
                                                   ? Theme.colorHover : "transparent"
                                            opacity: isAvailable ? 1.0 : 0.55

                                            readonly property bool selected:
                                                root.selectedConnectionId === connectionId

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 8
                                                spacing: 8

                                                Rectangle {
                                                    width: 18
                                                    height: 18
                                                    radius: 3
                                                    color: connRow.selected
                                                           ? Theme.colorAccentBlue : "transparent"
                                                    border.width: 2
                                                    border.color: connRow.selected
                                                                  ? Theme.colorAccentBlue
                                                                  : Theme.colorTextGrey
                                                    Text {
                                                        anchors.centerIn: parent
                                                        text: connRow.selected ? "\u2713" : ""
                                                        color: "white"
                                                        font.pixelSize: 11
                                                        font.bold: true
                                                    }
                                                }

                                                Rectangle {
                                                    width: 24
                                                    height: 24
                                                    radius: 4
                                                    color: "#4a9eff"
                                                    Text {
                                                        anchors.centerIn: parent
                                                        text: "\uD83D\uDCBB"
                                                        font.pixelSize: 12
                                                    }
                                                }

                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 1
                                                    Text {
                                                        text: displayName
                                                        color: Theme.colorTextWhite
                                                        font.pixelSize: 12
                                                        font.bold: true
                                                        font.family: Theme.fontFamily
                                                        elide: Text.ElideRight
                                                        Layout.fillWidth: true
                                                    }
                                                    Text {
                                                        text: (isDefault
                                                               ? qsTrId("aegra.backup.connection.default")
                                                                 + " · " : "") + stateText
                                                        color: isAvailable ? Theme.colorTextGrey
                                                                           : Theme.colorAccentRed
                                                        font.pixelSize: 11
                                                        font.family: Theme.fontFamily
                                                        elide: Text.ElideRight
                                                        Layout.fillWidth: true
                                                    }
                                                }
                                            }

                                            MouseArea {
                                                id: locHover
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                enabled: isAvailable
                                                cursorShape: isAvailable
                                                             ? Qt.PointingHandCursor
                                                             : Qt.ArrowCursor
                                                onClicked: {
                                                    root.selectedConnectionId = connectionId
                                                    root.confirmChecked = false
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                spacing: 8
                                AppButton {
                                    //% "Add"
                                    text: qsTrId("aegra.common.add")
                                    enabled: false
                                }
                                AppButton {
                                    //% "Import"
                                    text: qsTrId("aegra.common.import")
                                    enabled: false
                                }
                            }
                        }
                    }
                }

                // Options strip
                Card {
                    Layout.fillWidth: true
                    Layout.preferredHeight: optionsInner.implicitHeight + 56
                    //% "Backup options"
                    title: qsTrId("aegra.backup.section.options")

                    ColumnLayout {
                        id: optionsInner
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: 44
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16
                        spacing: 10

                        Row {
                            spacing: 8
                            Rectangle {
                                width: Math.max(90, fullLabel.implicitWidth + 28)
                                height: 34
                                radius: 4
                                color: Theme.colorAccentBlue
                                Text {
                                    id: fullLabel
                                    anchors.centerIn: parent
                                    //% "Full"
                                    text: qsTrId("aegra.backup.type.full")
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                }
                            }
                            Rectangle {
                                width: Math.max(90, incLabel.implicitWidth + 28)
                                height: 34
                                radius: 4
                                color: Theme.colorButtonDisabled
                                border.width: 1
                                border.color: Theme.colorBorder
                                opacity: 0.7
                                Text {
                                    id: incLabel
                                    anchors.centerIn: parent
                                    //% "Incremental"
                                    text: qsTrId("aegra.backup.type.incremental")
                                    color: Theme.colorButtonDisabledText
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                }
                            }
                            Rectangle {
                                width: Math.max(90, diffLabel.implicitWidth + 28)
                                height: 34
                                radius: 4
                                color: Theme.colorButtonDisabled
                                border.width: 1
                                border.color: Theme.colorBorder
                                opacity: 0.7
                                Text {
                                    id: diffLabel
                                    anchors.centerIn: parent
                                    //% "Differential"
                                    text: qsTrId("aegra.backup.type.differential")
                                    color: Theme.colorButtonDisabledText
                                    font.pixelSize: 13
                                    font.family: Theme.fontFamily
                                }
                            }
                        }

                        Text {
                            //% "Incremental backup is unavailable until Service returns eligible parents"
                            text: qsTrId("aegra.backup.incremental.unavailable")
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Text {
                            //% "Repository credentials are managed by Service. Passwords are never entered in Desktop."
                            text: qsTrId("aegra.backup.credential.service_managed")
                            color: Theme.colorTextDim
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        CheckBox {
                            id: confirmBox
                            //% "I understand this will start a full backup of the selected source"
                            text: qsTrId("aegra.backup.confirm.checkbox")
                            checked: root.confirmChecked
                            onCheckedChanged: root.confirmChecked = checked
                            indicator: Rectangle {
                                implicitWidth: 18
                                implicitHeight: 18
                                x: confirmBox.leftPadding
                                y: parent.height / 2 - height / 2
                                radius: 3
                                color: confirmBox.checked ? Theme.colorAccentBlue
                                                          : Theme.colorInput
                                border.width: 1
                                border.color: confirmBox.checked ? Theme.colorAccentBlue
                                                                 : Theme.colorBorder
                                Text {
                                    anchors.centerIn: parent
                                    text: confirmBox.checked ? "\u2713" : ""
                                    color: Theme.colorTextWhite
                                    font.pixelSize: 12
                                    font.bold: true
                                }
                            }
                            contentItem: Text {
                                text: confirmBox.text
                                color: Theme.colorTextWhite
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                leftPadding: confirmBox.indicator.width + 8
                                verticalAlignment: Text.AlignVCenter
                                wrapMode: Text.WordWrap
                            }
                        }

                        Text {
                            visible: serviceClient.backupCommandErrorText.length > 0
                            text: serviceClient.backupCommandErrorText
                            color: Theme.colorAccentRed
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        // Active progress inside wizard
                        ColumnLayout {
                            visible: serviceClient.activeBackupJobId.length > 0
                                     || serviceClient.backupCommandBusy
                            Layout.fillWidth: true
                            spacing: 6
                            Text {
                                text: serviceClient.activeBackupStateText
                                color: Theme.colorTextWhite
                                font.pixelSize: 13
                                font.family: Theme.fontFamily
                            }
                            Text {
                                visible: serviceClient.activeBackupJobId.length > 0
                                //% "Job %1"
                                text: qsTrId("aegra.backup.job_id")
                                      .arg(serviceClient.activeBackupJobId)
                                color: Theme.colorTextDim
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                            }
                            TaskProgressBar {
                                Layout.fillWidth: true
                                value: serviceClient.activeBackupProgressPercent
                                active: !serviceClient.activeBackupTerminal
                            }
                        }
                    }
                }

                // Footer actions — old GUI button chrome
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Item { Layout.fillWidth: true }

                    AppButton {
                        //% "Cancel"
                        text: qsTrId("aegra.common.cancel")
                        Layout.preferredWidth: 120
                        Layout.preferredHeight: 40
                        onClicked: root.closeWizard()
                    }
                    AppButton {
                        //% "Cancel job"
                        text: qsTrId("aegra.backup.action.cancel")
                        danger: true
                        Layout.preferredWidth: 120
                        Layout.preferredHeight: 40
                        visible: serviceClient.activeBackupCancellable
                        enabled: !serviceClient.cancelCommandBusy
                        onClicked: serviceClient.cancelActiveBackup()
                    }
                    AppButton {
                        //% "Start backup"
                        text: qsTrId("aegra.backup.action.start")
                        primary: true
                        Layout.preferredWidth: 140
                        Layout.preferredHeight: 40
                        enabled: root.fullBackupReady
                        onClicked: serviceClient.startBackup(root.selectedSourceId,
                                                             root.selectedConnectionId)
                    }
                }
            }
        }
    }

    Connections {
        target: serviceClient
        function onConnectionsChanged() {
            if (root.selectedConnectionId.length === 0) {
                const id = serviceClient.defaultConnectionId()
                if (id.length > 0)
                    root.selectedConnectionId = id
            }
        }
    }
}
