import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Item {
    id: root
    property bool recoveryPointDrawerOpen: false
    readonly property string selectedId: serviceClient.selectedRepositoryConnectionId
    readonly property bool repositorySelected: selectedId.length > 0
    property string selectedRecoveryPointId: ""
    property string selectedRecoveryPointSummary: ""

    function openConnectionDialog(importExisting) {
        connectionDialog.importExisting = importExisting
        connectionName.text = ""
        connectionLocator.text = ""
        connectionDialog.open()
        connectionName.forceActiveFocus()
    }

    function selectRecoveryPoint(fileUuid, summaryText) {
        root.selectedRecoveryPointId = fileUuid || ""
        root.selectedRecoveryPointSummary = summaryText || ""
    }

    function requestDeletePlan() {
        if (!root.selectedRecoveryPointId || root.selectedRecoveryPointId.length === 0)
            return
        if (!serviceClient.planDeleteRecoveryPoint(root.selectedRecoveryPointId)) {
            serviceClient.showToast(qsTrId("aegra.repository.delete.plan_failed"), true)
        }
    }

    function confirmExecuteDelete() {
        if (!serviceClient.executeDeletePlan()) {
            serviceClient.showToast(qsTrId("aegra.repository.delete.execute_failed"), true)
        }
    }

    Connections {
        target: serviceClient
        function onDeletePlanReady() {
            deletePlanDialog.open()
        }
        function onDeletePlanFailed(message) {
            serviceClient.showToast(message, true)
        }
        function onDeleteExecuted() {
            root.selectedRecoveryPointId = ""
            root.selectedRecoveryPointSummary = ""
            //% "Recovery points deleted"
            serviceClient.showToast(qsTrId("aegra.repository.delete.done"), false)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        // Title row matches old RepositoryPage (22px title, no accent bar)
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Text {
                //% "Repository"
                text: qsTrId("aegra.repository.title")
                color: Theme.colorTextWhite
                font.family: Theme.fontFamily
                font.pixelSize: 22
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            AppButton {
                //% "Refresh"
                //% "Reconnect"
                text: serviceClient.connected
                      ? qsTrId("aegra.common.refresh")
                      : qsTrId("aegra.common.reconnect")
                enabled: !serviceClient.repositoryLoading
                onClicked: {
                    if (serviceClient.connected) {
                        serviceClient.refreshConnections()
                        serviceClient.refreshRepository()
                    } else {
                        serviceClient.reconnect()
                    }
                }
            }
            AppButton {
                //% "Add repository"
                text: qsTrId("aegra.repository.add")
                primary: true
                enabled: serviceClient.connected && !serviceClient.repositoryCommandBusy
                onClicked: root.openConnectionDialog(false)
            }
            AppButton {
                //% "Import..."
                text: qsTrId("aegra.repository.import")
                enabled: serviceClient.connected && !serviceClient.repositoryCommandBusy
                onClicked: root.openConnectionDialog(true)
            }
        }

        Text {
            Layout.fillWidth: true
            //% "Loaded %1 repositories"
            text: qsTrId("aegra.repository.loaded_count").arg(serviceClient.connections.count)
            color: Theme.colorTextGrey
            font.family: Theme.fontFamily
            font.pixelSize: 12
        }

        ListView {
            id: repositoryList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: serviceClient.connections
            spacing: 8

            delegate: Rectangle {
                required property string connectionId
                required property string displayName
                required property string stateText
                required property bool isDefault
                required property bool isAvailable
                required property var capabilities
                width: repositoryList.width
                height: 96
                radius: 4
                color: connectionId === root.selectedId ? Theme.colorHover : Theme.colorCard
                border.width: 1
                border.color: connectionId === root.selectedId
                              ? Theme.colorAccentBlue : Theme.colorBorder

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: serviceClient.selectRepositoryConnection(connectionId)
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true

                        Text {
                            Layout.fillWidth: true
                            text: displayName
                            color: Theme.colorTextWhite
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Rectangle {
                            z: 2
                            Layout.preferredWidth: defaultLabel.implicitWidth + 14
                            Layout.preferredHeight: 22
                            radius: 3
                            color: Theme.colorButton
                            border.width: 1
                            border.color: Theme.colorBorder

                            Text {
                                id: defaultLabel
                                anchors.centerIn: parent
                                //% "Default"
                                text: qsTrId("aegra.backup.connection.default")
                                color: Theme.colorTextWhite
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.bold: true
                            }

                            visible: isDefault
                        }

                        Text {
                            //% "connected"
                            text: stateText
                            color: isAvailable ? Theme.colorGreen : Theme.colorTextGrey
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: connectionId
                        color: Theme.colorTextGrey
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }

                    Text {
                        Layout.fillWidth: true
                        text: capabilities.join(" · ")
                        color: Theme.colorTextDim
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        visible: capabilities.length > 0
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: repositoryList.count === 0 && !serviceClient.connectionsLoading
                text: !serviceClient.connected
                      ? serviceClient.statusText
                      : (serviceClient.connectionsErrorText.length > 0
                         ? serviceClient.connectionsErrorText
                         : qsTrId("aegra.repository.empty"))
                color: Theme.colorTextGrey
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            enabled: root.repositorySelected
            opacity: enabled ? 1.0 : 0.5

            AppButton {
                //% "Set default"
                text: qsTrId("aegra.repository.set_default")
                enabled: serviceClient.connected && !serviceClient.repositoryCommandBusy
                onClicked: serviceClient.setDefaultRepositoryConnection(root.selectedId)
            }
            AppButton {
                //% "Test"
                text: qsTrId("aegra.repository.test")
                enabled: serviceClient.connected && !serviceClient.repositoryCommandBusy
                onClicked: serviceClient.testRepositoryConnection(root.selectedId)
            }
            AppButton {
                text: qsTrId("aegra.repository.recovery_points_count")
                      .arg(serviceClient.recoveryPointCount)
                enabled: serviceClient.repositoryConfigured && !serviceClient.repositoryLoading
                onClicked: root.recoveryPointDrawerOpen = true
            }
            Item { Layout.fillWidth: true }
            AppButton {
                //% "Delete"
                text: qsTrId("aegra.common.delete")
                danger: true
                enabled: serviceClient.connected && !serviceClient.repositoryCommandBusy
                onClicked: removeDialog.open()
            }
        }

        Text {
            Layout.fillWidth: true
            visible: serviceClient.repositoryCommandErrorText.length > 0
            text: serviceClient.repositoryCommandErrorText
            color: Theme.colorAccentRed
            font.family: Theme.fontFamily
            font.pixelSize: 11
        }
    }

    Dialog {
        id: connectionDialog
        property bool importExisting: false
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: 440
        title: importExisting ? qsTrId("aegra.repository.import")
                              : qsTrId("aegra.repository.add")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            if (importExisting)
                serviceClient.importRepositoryConnection(connectionName.text,
                                                         connectionLocator.text)
            else
                serviceClient.addRepositoryConnection(connectionName.text,
                                                      connectionLocator.text)
        }
        onOpened: standardButton(Dialog.Ok).enabled = Qt.binding(function() {
            return connectionName.text.trim().length > 0
                    && connectionLocator.text.trim().length > 0
        })

        background: Rectangle {
            color: Theme.colorCard
            border.width: 1
            border.color: Theme.colorBorder
            radius: 4
        }

        contentItem: ColumnLayout {
            spacing: 10
            Text {
                //% "Display name"
                text: qsTrId("aegra.repository.display_name")
                color: Theme.colorTextGrey
                font.family: Theme.fontFamily
            }
            TextField {
                id: connectionName
                Layout.fillWidth: true
                selectByMouse: true
            }
            Text {
                //% "Repository location"
                text: qsTrId("aegra.repository.location")
                color: Theme.colorTextGrey
                font.family: Theme.fontFamily
            }
            TextField {
                id: connectionLocator
                Layout.fillWidth: true
                selectByMouse: true
                placeholderText: "D:\\Backup"
            }
        }
    }

    Dialog {
        id: removeDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: 420
        //% "Remove repository connection?"
        title: qsTrId("aegra.repository.remove_title")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: serviceClient.removeRepositoryConnection(root.selectedId)

        background: Rectangle {
            color: Theme.colorCard
            border.width: 1
            border.color: Theme.colorBorder
            radius: 4
        }

        contentItem: Text {
            //% "Only the saved connection is removed. Backup data is not deleted."
            text: qsTrId("aegra.repository.remove_description")
            color: Theme.colorTextGrey
            font.family: Theme.fontFamily
            wrapMode: Text.WordWrap
        }
    }

    Item {
        id: recoveryPointDrawer
        anchors.fill: parent
        z: 2100
        enabled: root.recoveryPointDrawerOpen || drawerPanel.slideProgress < 0.999

        Rectangle {
            anchors.fill: parent
            color: Theme.colorScrim
            opacity: root.recoveryPointDrawerOpen ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 250 } }
            MouseArea {
                anchors.fill: parent
                enabled: root.recoveryPointDrawerOpen
            }
        }

        Rectangle {
            id: drawerPanel
            width: Math.max(560, parent.width * 0.9)
            height: parent.height
            y: 0
            property real slideProgress: root.recoveryPointDrawerOpen ? 0 : 1
            x: parent.width - width + slideProgress * width
            visible: slideProgress < 0.999 || root.recoveryPointDrawerOpen
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
                    Layout.preferredHeight: 32
                    spacing: 8

                    Rectangle {
                        Layout.preferredWidth: 3
                        Layout.preferredHeight: 18
                        color: Theme.colorAccentBlue
                    }

                    Text {
                        Layout.fillWidth: true
                        //% "Personal Repository — Recovery Points"
                        text: qsTrId("aegra.repository.drawer_title")
                        color: Theme.colorTextWhite
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.bold: true
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
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: root.recoveryPointDrawerOpen = false
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    AppButton {
                        //% "Refresh"
                        text: qsTrId("aegra.common.refresh")
                        enabled: serviceClient.connected && !serviceClient.repositoryLoading
                        onClicked: serviceClient.refreshRepository()
                    }
                    AppButton {
                        //% "Delete"
                        text: qsTrId("aegra.common.delete")
                        danger: true
                        enabled: serviceClient.connected
                                 && root.selectedRecoveryPointId.length > 0
                                 && !serviceClient.deletePlanBusy
                                 && !serviceClient.repositoryLoading
                        onClicked: root.requestDeletePlan()
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        visible: root.selectedRecoveryPointId.length > 0
                        text: root.selectedRecoveryPointSummary
                        color: Theme.colorTextGrey
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        Layout.maximumWidth: 220
                    }
                    Text {
                        //% "%1 recovery points"
                        text: qsTrId("aegra.repository.recovery_points_count")
                              .arg(serviceClient.recoveryPointCount)
                        color: Theme.colorTextDim
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    color: Theme.colorTableHeader

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 10

                        Item { Layout.preferredWidth: 28 }
                        Text {
                            Layout.fillWidth: true
                            //% "Recovery point"
                            text: qsTrId("aegra.repository.column.recovery_point")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                        Text {
                            Layout.preferredWidth: 120
                            //% "Backup time"
                            text: qsTrId("aegra.repository.column.backup_time")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                        Text {
                            Layout.preferredWidth: 60
                            //% "Type"
                            text: qsTrId("aegra.repository.column.type")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                        Text {
                            Layout.preferredWidth: 80
                            //% "Logical size"
                            text: qsTrId("aegra.repository.column.logical_size")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                        Text {
                            visible: drawerPanel.width >= 800
                            Layout.preferredWidth: 80
                            //% "Stored size"
                            text: qsTrId("aegra.repository.column.stored_size")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                        Text {
                            Layout.preferredWidth: 72
                            //% "Backup chain"
                            text: qsTrId("aegra.repository.column.chain")
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                    }
                }

                ListView {
                    id: recoveryPointList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: serviceClient.recoveryPoints

                    delegate: Rectangle {
                        required property string fileUuid
                        required property string backupSetUuid
                        required property string createdText
                        required property string backupTypeText
                        required property string logicalSizeText
                        required property string storedSizeText
                        required property string chainStateText
                        required property bool chainComplete
                        required property string parentSummaryText
                        required property int chainDepth
                        required property bool isBaseline
                        required property int index
                        readonly property bool selected: root.selectedRecoveryPointId === fileUuid
                        width: recoveryPointList.width
                        height: 64
                        color: selected
                               ? Qt.rgba(Theme.colorAccentBlue.r, Theme.colorAccentBlue.g,
                                         Theme.colorAccentBlue.b, 0.18)
                               : (index % 2 === 0 ? Theme.colorTableRow : Theme.colorTableAlt)

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 10

                            Rectangle {
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 16
                                Layout.leftMargin: 6
                                radius: 3
                                color: selected ? Theme.colorAccentBlue : "transparent"
                                border.width: selected ? 0 : 1
                                border.color: Theme.colorBorder
                                Text {
                                    anchors.centerIn: parent
                                    visible: selected
                                    text: "\u2713"
                                    color: "white"
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                            }

                            Column {
                                Layout.fillWidth: true
                                spacing: 2

                                Text {
                                    width: parent.width
                                    text: fileUuid
                                    color: Theme.colorTextWhite
                                    font.family: "Consolas"
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }
                                Text {
                                    width: parent.width
                                    text: parentSummaryText
                                          + (chainDepth > 1
                                             ? (" · " + qsTrId("aegra.repository.chain.depth")
                                                .arg(chainDepth))
                                             : "")
                                    color: Theme.colorTextDim
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                }
                            }

                            Text {
                                Layout.preferredWidth: 120
                                text: createdText
                                color: Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.preferredWidth: 60
                                text: backupTypeText
                                color: isBaseline ? Theme.colorTextWhite : Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.bold: isBaseline
                            }
                            Text {
                                Layout.preferredWidth: 80
                                text: logicalSizeText
                                color: Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                            }
                            Text {
                                visible: drawerPanel.width >= 800
                                Layout.preferredWidth: 80
                                text: storedSizeText
                                color: Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.preferredWidth: 72
                                text: chainStateText
                                color: chainComplete
                                       ? Theme.colorGreen : Theme.colorAccentRed
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.bold: true
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectRecoveryPoint(
                                           fileUuid,
                                           backupTypeText + " · " + createdText)
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: Theme.colorBorder
                            opacity: 0.5
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: recoveryPointList.count === 0 && !serviceClient.repositoryLoading
                        //% "No recovery points"
                        text: qsTrId("aegra.repository.empty_recovery_points")
                        color: Theme.colorTextGrey
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                    }
                }
            }
        }
    }

    // Server-authored delete plan confirmation (target count only; no local dependency calc).
    Dialog {
        id: deletePlanDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(440, parent.width - 40)
        //% "Delete recovery points"
        title: qsTrId("aegra.repository.delete.title")
        standardButtons: Dialog.NoButton
        background: Rectangle {
            color: Theme.colorPopup
            border.color: Theme.colorBorder
            radius: 8
        }
        contentItem: ColumnLayout {
            spacing: 14
            width: deletePlanDialog.availableWidth
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                //% "The service planned to delete %1 recovery point(s) in this chain. Other recovery points are kept. This cannot be undone."
                text: qsTrId("aegra.repository.delete.plan_message")
                      .arg((serviceClient.deletePlan && serviceClient.deletePlan.targetCount)
                           ? serviceClient.deletePlan.targetCount : 0)
                color: Theme.colorTextWhite
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }
            Text {
                Layout.fillWidth: true
                visible: serviceClient.deletePlan
                         && serviceClient.deletePlan.retainedCount !== undefined
                wrapMode: Text.WordWrap
                //% "Approximately %1 other recovery point(s) currently listed will remain."
                text: qsTrId("aegra.repository.delete.retained_hint")
                      .arg((serviceClient.deletePlan
                            && serviceClient.deletePlan.retainedCount !== undefined)
                           ? serviceClient.deletePlan.retainedCount : 0)
                color: Theme.colorTextGrey
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Item { Layout.fillWidth: true }
                AppButton {
                    //% "Cancel"
                    text: qsTrId("aegra.common.cancel")
                    onClicked: {
                        serviceClient.clearDeletePlan()
                        deletePlanDialog.close()
                    }
                }
                AppButton {
                    //% "Delete permanently"
                    text: qsTrId("aegra.repository.delete.confirm")
                    danger: true
                    enabled: !serviceClient.deletePlanBusy
                    onClicked: {
                        deletePlanDialog.close()
                        root.confirmExecuteDelete()
                    }
                }
            }
        }
    }
}
