import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "."

ApplicationWindow {
    id: window
    visible: true
    width: 1100
    height: 700
    minimumWidth: 860
    minimumHeight: 560
    title: "Aegra"
    color: Theme.workspace

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

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 212
            color: Theme.sidebar

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 20
                    spacing: 10

                    Image {
                        source: "qrc:/Aegra/icons/product_32.png"
                        sourceSize.width: 30
                        sourceSize.height: 30
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 30
                    }

                    Text {
                        text: "Aegra"
                        color: "#ffffff"
                        font.family: Theme.fontFamily
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }
                }

                Repeater {
                    model: [
                        { label: "恢复点", glyph: "\uE81E", enabled: true },
                        { label: "备份", glyph: "\uEA35", enabled: false },
                        { label: "恢复", glyph: "\uE965", enabled: false },
                        { label: "存储库", glyph: "\uE8B7", enabled: false }
                    ]

                    delegate: Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 42
                        radius: 4
                        color: modelData.enabled ? Theme.sidebarHover : "transparent"
                        opacity: modelData.enabled ? 1.0 : 0.48

                        Row {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 12

                            Text {
                                text: modelData.glyph
                                color: "#ffffff"
                                font.family: "Segoe MDL2 Assets"
                                font.pixelSize: 17
                            }

                            Text {
                                text: modelData.label
                                color: "#ffffff"
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 9

                    Rectangle {
                        Layout.preferredWidth: 9
                        Layout.preferredHeight: 9
                        radius: 4
                        color: serviceClient.connected ? Theme.success : Theme.danger
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "Service " + serviceClient.statusText
                        color: "#dfe3e8"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 30
                anchors.rightMargin: 30
                anchors.topMargin: 24
                anchors.bottomMargin: 24
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true

                    Column {
                        spacing: 3

                        Text {
                            text: "恢复点"
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 24
                            font.weight: Font.DemiBold
                        }

                        Text {
                            text: serviceClient.repositoryStatusText
                            color: serviceClient.repositoryErrorText.length > 0
                                   ? Theme.danger : Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        id: reconnectButton
                        visible: !serviceClient.connected
                        text: "重新连接"
                        onClicked: serviceClient.reconnect()

                        contentItem: Text {
                            text: reconnectButton.text
                            color: "#ffffff"
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        background: Rectangle {
                            radius: 4
                            color: reconnectButton.down ? "#1f65b4" : Theme.accent
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 68
                    radius: 6
                    color: Theme.surface
                    border.color: Theme.border
                    border.width: 1

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 18
                        anchors.rightMargin: 18
                        spacing: 12

                        Rectangle {
                            Layout.preferredWidth: 34
                            Layout.preferredHeight: 34
                            radius: 4
                            color: serviceClient.connected ? Theme.successSoft : Theme.dangerSoft

                            Text {
                                anchors.centerIn: parent
                                text: serviceClient.connected ? "\uE930" : "\uE783"
                                color: serviceClient.connected ? Theme.success : Theme.danger
                                font.family: "Segoe MDL2 Assets"
                                font.pixelSize: 16
                            }
                        }

                        Column {
                            spacing: 2

                            Text {
                                text: "本机管理服务"
                                color: Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                            }

                            Text {
                                text: serviceClient.errorText.length > 0
                                      ? serviceClient.errorText : serviceClient.statusText
                                color: serviceClient.errorText.length > 0
                                       ? Theme.danger : Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                        }

                        Item { Layout.fillWidth: true }

                        Text {
                            visible: serviceClient.connected
                            text: "Service " + serviceClient.serviceVersion
                                  + "  |  API " + serviceClient.apiVersion
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Rectangle {
                        Layout.preferredWidth: 8
                        Layout.preferredHeight: 8
                        radius: 4
                        color: serviceClient.repositoryErrorText.length > 0
                               ? Theme.danger
                               : (serviceClient.repositoryConfigured ? Theme.success : Theme.textMuted)
                    }

                    Text {
                        text: "个人版 Repository"
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }

                    Text {
                        Layout.fillWidth: true
                        text: serviceClient.repositoryUuid
                        visible: text.length > 0
                        color: Theme.textMuted
                        font.family: "Consolas"
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                        horizontalAlignment: Text.AlignRight
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: serviceClient.repositoryErrorText.length > 0
                    text: serviceClient.repositoryErrorText
                    color: Theme.danger
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }

                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 24
                    running: serviceClient.repositoryLoading
                    visible: running
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 36
                    visible: serviceClient.connected && !serviceClient.repositoryLoading
                             && !serviceClient.repositoryConfigured
                             && serviceClient.repositoryErrorText.length === 0
                    text: "未配置个人版 Repository"
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                }

                Rectangle {
                    visible: serviceClient.repositoryConfigured
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    color: "#e9edf2"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        spacing: 12

                        Text { Layout.fillWidth: true; text: "恢复点"; color: Theme.textMuted; font.pixelSize: 11 }
                        Text { Layout.preferredWidth: 56; text: "类型"; color: Theme.textMuted; font.pixelSize: 11 }
                        Text { Layout.preferredWidth: 116; text: "创建时间"; color: Theme.textMuted; font.pixelSize: 11 }
                        Text { Layout.preferredWidth: 74; text: "逻辑大小"; color: Theme.textMuted; font.pixelSize: 11 }
                        Text {
                            visible: window.width >= 1000
                            Layout.preferredWidth: 74
                            text: "存储大小"
                            color: Theme.textMuted
                            font.pixelSize: 11
                        }
                        Item { Layout.preferredWidth: 108 }
                    }
                }

                ListView {
                    visible: serviceClient.repositoryConfigured
                             && serviceClient.recoveryPoints.length > 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: serviceClient.recoveryPoints
                    spacing: 1
                    clip: true

                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: ListView.view.width
                        height: 68
                        color: index % 2 === 0 ? Theme.surface : "#f8f9fb"
                        border.color: Theme.border
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 12

                            Column {
                                Layout.fillWidth: true
                                spacing: 3

                                Text {
                                    width: parent.width
                                    text: modelData.fileUuid
                                    color: Theme.text
                                    font.family: "Consolas"
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }

                                Text {
                                    width: parent.width
                                    text: modelData.chainState === 1 ? "链完整" : "链不完整"
                                    color: modelData.chainState === 1 ? Theme.success : Theme.danger
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 11
                                }
                            }

                            Text {
                                Layout.preferredWidth: 56
                                text: window.backupTypeLabel(modelData.backupType)
                                color: Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }

                            Text {
                                Layout.preferredWidth: 116
                                text: window.formatDate(modelData.createdUtcMs)
                                color: Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }

                            Text {
                                Layout.preferredWidth: 74
                                text: window.formatBytes(modelData.logicalSizeBytes)
                                color: Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }

                            Text {
                                visible: window.width >= 1000
                                Layout.preferredWidth: 74
                                text: window.formatBytes(modelData.storedSizeBytes)
                                color: Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }

                            Row {
                                Layout.preferredWidth: 108
                                spacing: 4

                                Repeater {
                                    model: [
                                        { glyph: "\uE777", tip: "恢复" },
                                        { glyph: "\uE73E", tip: "校验" },
                                        { glyph: "\uE74D", tip: "删除" }
                                    ]

                                    delegate: Button {
                                        required property var modelData
                                        width: 32
                                        height: 32
                                        enabled: false
                                        opacity: 0.45

                                        contentItem: Text {
                                            text: modelData.glyph
                                            color: Theme.textMuted
                                            font.family: "Segoe MDL2 Assets"
                                            font.pixelSize: 14
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }

                                        background: Rectangle {
                                            radius: 4
                                            color: Theme.workspace
                                            border.color: Theme.border
                                        }

                                        ToolTip.visible: hovered
                                        ToolTip.text: modelData.tip
                                    }
                                }
                            }
                        }
                    }
                }

                Item {
                    visible: serviceClient.repositoryConfigured
                             && serviceClient.recoveryPoints.length === 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Text {
                        anchors.centerIn: parent
                        text: "Repository 中没有恢复点"
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                    }
                }

                Item {
                    visible: !serviceClient.repositoryConfigured
                    Layout.fillHeight: true
                }
            }
        }
    }
}
