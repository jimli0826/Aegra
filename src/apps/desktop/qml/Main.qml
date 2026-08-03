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
                        { label: "概览", glyph: "\uE80F", enabled: true },
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
                anchors.leftMargin: 34
                anchors.rightMargin: 34
                anchors.topMargin: 28
                anchors.bottomMargin: 28
                spacing: 18

                RowLayout {
                    Layout.fillWidth: true

                    Column {
                        spacing: 4

                        Text {
                            text: "概览"
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 26
                            font.weight: Font.DemiBold
                        }

                        Text {
                            text: "本机管理服务"
                            color: Theme.textMuted
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
                    Layout.preferredHeight: serviceClient.connected ? 236 : 176
                    radius: 6
                    color: Theme.surface
                    border.color: Theme.border
                    border.width: 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 22
                        spacing: 14

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            Rectangle {
                                Layout.preferredWidth: 40
                                Layout.preferredHeight: 40
                                radius: 5
                                color: serviceClient.connected ? Theme.successSoft : Theme.dangerSoft

                                Text {
                                    anchors.centerIn: parent
                                    text: serviceClient.connected ? "\uE930" : "\uE783"
                                    color: serviceClient.connected ? Theme.success : Theme.danger
                                    font.family: "Segoe MDL2 Assets"
                                    font.pixelSize: 18
                                }
                            }

                            Column {
                                spacing: 3

                                Text {
                                    text: serviceClient.statusText
                                    color: Theme.text
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 17
                                    font.weight: Font.DemiBold
                                }

                                Text {
                                    text: serviceClient.errorText
                                    visible: text.length > 0
                                    color: Theme.danger
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: Theme.border
                        }

                        GridLayout {
                            visible: serviceClient.connected
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 28
                            rowSpacing: 10

                            Text { text: "Service 版本"; color: Theme.textMuted; font.pixelSize: 12 }
                            Text { text: serviceClient.serviceVersion; color: Theme.text; font.pixelSize: 13 }
                            Text { text: "API 版本"; color: Theme.textMuted; font.pixelSize: 12 }
                            Text { text: serviceClient.apiVersion.toString(); color: Theme.text; font.pixelSize: 13 }
                            Text { text: "连接端点"; color: Theme.textMuted; font.pixelSize: 12 }
                            Text { text: "aegra-service-control"; color: Theme.text; font.pixelSize: 13 }
                        }
                    }
                }

                Text {
                    visible: serviceClient.connected
                    text: "可用能力"
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }

                ListView {
                    visible: serviceClient.connected
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: serviceClient.capabilities
                    spacing: 1
                    clip: true

                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 42
                        color: index % 2 === 0 ? Theme.surface : "#f8f9fb"
                        border.color: Theme.border
                        border.width: 1

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                        }
                    }
                }

                Item { visible: !serviceClient.connected; Layout.fillHeight: true }
            }
        }
    }
}
