import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Item {
    id: root
    property bool recoveryPointDrawerOpen: false
    property bool repositorySelected: serviceClient.repositoryConfigured

    function formatBytes(value) {
        const units = ["B", "KiB", "MiB", "GiB", "TiB"]
        let amount = Number(value)
        let unit = 0
        while (amount >= 1024 && unit < units.length - 1) {
            amount /= 1024
            ++unit
        }
        return (unit === 0 ? amount.toFixed(0) : amount.toFixed(1)) + " " + units[unit]
    }

    function formatDate(value) {
        if (Number(value) === 0)
            return "未记录"
        return new Date(Number(value)).toLocaleString(Qt.locale(), "yyyy-MM-dd hh:mm")
    }

    function backupTypeLabel(value) {
        if (value === 1)
            return "全量"
        if (value === 2)
            return "增量"
        return "差异"
    }

    readonly property var repositoryModel: serviceClient.repositoryConfigured ? [{
        name: "个人版 Repository",
        repoUuid: serviceClient.repositoryUuid,
        checkpointCount: serviceClient.recoveryPoints.length,
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
                text: "Repository"
                color: Theme.colorTextWhite
                font.family: Theme.fontFamily
                font.pixelSize: 22
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            AppButton {
                text: serviceClient.connected ? "刷新" : "重新连接"
                enabled: !serviceClient.repositoryLoading
                onClicked: serviceClient.connected
                           ? serviceClient.refreshRepository() : serviceClient.reconnect()
            }
            AppButton { text: "添加"; primary: true; enabled: false }
            AppButton { text: "导入"; enabled: false }
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
                                text: modelData.checkpointCount + " 个恢复点"
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
                        text: "local · catalog"
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
                text: "没有 Repository"
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

            AppButton { text: "设为默认"; enabled: false }
            AppButton { text: "连接测试"; enabled: false }
            AppButton { text: "解锁"; enabled: false }
            AppButton { text: "锁定"; enabled: false }
            AppButton { text: "重建索引"; enabled: false }
            AppButton { text: "导出"; enabled: false }
            AppButton { text: "设置密码"; enabled: false }
            Item { Layout.fillWidth: true }
            AppButton { text: "删除"; danger: true; enabled: false }
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
                        text: "个人版 Repository — 恢复点"
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
                        text: "刷新"
                        enabled: serviceClient.connected && !serviceClient.repositoryLoading
                        onClicked: serviceClient.refreshRepository()
                    }
                    AppButton { text: "删除"; danger: true; enabled: false }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: serviceClient.recoveryPoints.length + " 个恢复点"
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
                        Text { Layout.fillWidth: true; text: "恢复点"; color: Theme.colorTextGrey; font.pixelSize: 11 }
                        Text { Layout.preferredWidth: 120; text: "备份时间"; color: Theme.colorTextGrey; font.pixelSize: 11 }
                        Text { Layout.preferredWidth: 60; text: "类型"; color: Theme.colorTextGrey; font.pixelSize: 11 }
                        Text { Layout.preferredWidth: 80; text: "逻辑大小"; color: Theme.colorTextGrey; font.pixelSize: 11 }
                        Text {
                            visible: drawerPanel.width >= 800
                            Layout.preferredWidth: 80
                            text: "存储大小"
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                        }
                        Text { Layout.preferredWidth: 72; text: "备份链"; color: Theme.colorTextGrey; font.pixelSize: 11 }
                    }
                }

                ListView {
                    id: recoveryPointList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: serviceClient.recoveryPoints

                    delegate: Rectangle {
                        required property var modelData
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
                                    text: modelData.fileUuid
                                    color: Theme.colorTextWhite
                                    font.family: "Consolas"
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }
                                Text {
                                    width: parent.width
                                    text: modelData.backupSetUuid
                                    color: Theme.colorTextDim
                                    font.family: "Consolas"
                                    font.pixelSize: 9
                                    elide: Text.ElideMiddle
                                }
                            }

                            Text {
                                Layout.preferredWidth: 120
                                text: root.formatDate(modelData.createdUtcMs)
                                color: Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.preferredWidth: 60
                                text: root.backupTypeLabel(modelData.backupType)
                                color: Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.preferredWidth: 80
                                text: root.formatBytes(modelData.logicalSizeBytes)
                                color: Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                            }
                            Text {
                                visible: drawerPanel.width >= 800
                                Layout.preferredWidth: 80
                                text: root.formatBytes(modelData.storedSizeBytes)
                                color: Theme.colorTextGrey
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.preferredWidth: 72
                                text: modelData.chainState === 1 ? "完整" : "不完整"
                                color: modelData.chainState === 1
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
                        text: "没有恢复点"
                        color: Theme.colorTextGrey
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                    }
                }
            }
        }
    }
}
