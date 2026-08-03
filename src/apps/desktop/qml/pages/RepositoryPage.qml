import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Item {
    id: root
    property bool recoveryPointDrawerOpen: false
    property bool repositorySelected: serviceClient.repositoryConfigured

    readonly property var repositoryModel: serviceClient.repositoryConfigured ? [{
        //% "Personal Repository"
        name: qsTrId("aegra.repository.personal_name"),
        repoUuid: serviceClient.repositoryUuid,
        checkpointCount: serviceClient.recoveryPointCount,
        status: serviceClient.repositoryStatusText
    }] : []

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

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
                onClicked: serviceClient.connected
                           ? serviceClient.refreshRepository() : serviceClient.reconnect()
            }
            AppButton {
                //% "Add"
                text: qsTrId("aegra.common.add")
                primary: true
                enabled: false
            }
            AppButton {
                //% "Import"
                text: qsTrId("aegra.common.import")
                enabled: false
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            BusyIndicator {
                running: serviceClient.repositoryLoading
                visible: running
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
            }

            Text {
                Layout.fillWidth: true
                text: serviceClient.repositoryErrorText.length > 0
                      ? serviceClient.repositoryErrorText : serviceClient.repositoryStatusText
                color: serviceClient.repositoryErrorText.length > 0
                       ? Theme.colorAccentRed : Theme.colorTextGrey
                font.family: Theme.fontFamily
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        ListView {
            id: repositoryList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.repositoryModel
            spacing: 8

            delegate: Rectangle {
                required property var modelData
                width: repositoryList.width
                height: 96
                radius: 4
                color: root.repositorySelected ? Theme.colorHover : Theme.colorCard
                border.width: 1
                border.color: root.repositorySelected
                              ? Theme.colorAccentBlue : Theme.colorBorder

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.repositorySelected = true
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true

                        Text {
                            Layout.fillWidth: true
                            text: modelData.name
                            color: Theme.colorTextWhite
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Rectangle {
                            z: 2
                            width: recoveryCount.implicitWidth + 14
                            height: 22
                            radius: 3
                            color: recoveryMouse.containsMouse
                                   ? Theme.colorAccentBlue : Theme.colorButton
                            border.width: 1
                            border.color: Theme.colorBorder

                            Text {
                                id: recoveryCount
                                anchors.centerIn: parent
                                //% "%1 recovery points"
                                text: qsTrId("aegra.repository.recovery_points_count")
                                      .arg(modelData.checkpointCount)
                                color: Theme.colorTextWhite
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.bold: true
                            }

                            MouseArea {
                                id: recoveryMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.recoveryPointDrawerOpen = true
                            }
                        }

                        Text {
                            text: modelData.status
                            color: Theme.colorTextGrey
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: modelData.repoUuid
                        color: Theme.colorTextGrey
                        font.family: "Consolas"
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }

                    Text {
                        Layout.fillWidth: true
                        //% "local · catalog"
                        text: qsTrId("aegra.repository.kind_local_catalog")
                        color: Theme.colorTextDim
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: repositoryList.count === 0 && !serviceClient.repositoryLoading
                         && serviceClient.repositoryErrorText.length === 0
                //% "No repository"
                text: qsTrId("aegra.repository.empty")
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
                enabled: false
            }
            AppButton {
                //% "Test connection"
                text: qsTrId("aegra.repository.test_connection")
                enabled: false
            }
            AppButton {
                //% "Unlock"
                text: qsTrId("aegra.repository.unlock")
                enabled: false
            }
            AppButton {
                //% "Lock"
                text: qsTrId("aegra.repository.lock")
                enabled: false
            }
            AppButton {
                //% "Rebuild index"
                text: qsTrId("aegra.repository.rebuild_index")
                enabled: false
            }
            AppButton {
                //% "Export"
                text: qsTrId("aegra.common.export")
                enabled: false
            }
            AppButton {
                //% "Set password"
                text: qsTrId("aegra.repository.set_password")
                enabled: false
            }
            Item { Layout.fillWidth: true }
            AppButton {
                //% "Delete"
                text: qsTrId("aegra.common.delete")
                danger: true
                enabled: false
            }
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
                        enabled: false
                    }
                    Item { Layout.fillWidth: true }
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
                        required property int index
                        width: recoveryPointList.width
                        height: 58
                        color: index % 2 === 0
                               ? Theme.colorTableRow : Theme.colorTableAlt

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 10

                            Rectangle {
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 16
                                Layout.leftMargin: 6
                                color: Theme.colorInput
                                border.width: 1
                                border.color: Theme.colorBorder
                                opacity: 0.5
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
                                    text: backupSetUuid
                                    color: Theme.colorTextDim
                                    font.family: "Consolas"
                                    font.pixelSize: 9
                                    elide: Text.ElideMiddle
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
                                color: Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
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
}
