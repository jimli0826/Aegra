import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

// Modal About dialog matching desktop style and reference Figure 2.
Popup {
    id: root
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    anchors.centerIn: Overlay.overlay
    width: Math.min(540, parent ? parent.width - 32 : 540)
    padding: 22

    signal licensesRequested()

    background: Rectangle {
        radius: Theme.radiusCard
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: Theme.colorCard }
            GradientStop { position: 1.0; color: Theme.colorCardEnd }
        }
        border.width: 1
        border.color: Theme.colorBorder
    }

    contentItem: ColumnLayout {
        spacing: 14

        // Dialog header with close button
        RowLayout {
            Layout.fillWidth: true

            Text {
                text: qsTrId("aegra.about.title")
                color: Theme.colorTextWhite
                font.pixelSize: 15
                font.bold: true
                font.family: Theme.fontFamily
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                width: 26
                height: 26
                radius: 6
                color: closeMouse.containsMouse ? Theme.colorHover : "transparent"

                Text {
                    anchors.centerIn: parent
                    text: "\u2715"
                    color: closeMouse.containsMouse ? Theme.colorHoverClose : Theme.colorTextGrey
                    font.pixelSize: 13
                    font.family: Theme.fontFamily
                }

                MouseArea {
                    id: closeMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.close()
                }
            }
        }

        // Product banner: Logo + Details
        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Image {
                Layout.preferredWidth: 56
                Layout.preferredHeight: 56
                source: "qrc:/Aegra/icons/product.png"
                smooth: true
                mipmap: true
                fillMode: Image.PreserveAspectFit
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: (typeof localeController !== "undefined" && localeController)
                              ? localeController.productName : "Aegra Image"
                        color: Theme.colorTextWhite
                        font.pixelSize: 18
                        font.bold: true
                        font.family: Theme.fontFamily
                    }

                    Text {
                        text: "(" + ((typeof localeController !== "undefined" && localeController)
                                     ? localeController.architecture : "64-bit") + ")"
                        color: Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                        Layout.alignment: Qt.AlignVCenter
                    }

                    Item { Layout.fillWidth: true }

                    // Built with Qt badge
                    Rectangle {
                        radius: 4
                        color: Theme.colorInput
                        border.width: 1
                        border.color: Theme.colorBorder
                        implicitHeight: 24
                        implicitWidth: qtBadgeRow.implicitWidth + 12

                        Row {
                            id: qtBadgeRow
                            anchors.centerIn: parent
                            spacing: 4

                            Text {
                                text: qsTrId("aegra.about.built_with")
                                color: Theme.colorTextDim
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                                anchors.verticalCenter: parent.verticalCenter
                            }

                            Text {
                                text: "Qt " + ((typeof localeController !== "undefined" && localeController)
                                               ? localeController.qtVersion : "6.8.3")
                                color: Theme.colorGreen
                                font.pixelSize: 11
                                font.bold: true
                                font.family: Theme.fontFamily
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                    }
                }

                Text {
                    text: qsTrId("aegra.about.version_for_windows")
                          .arg((typeof localeController !== "undefined" && localeController) ? localeController.productVersion : "0.9.0.2")
                          .arg((typeof localeController !== "undefined" && localeController) ? localeController.buildDate : "2026.08.27")
                    color: Theme.colorTextGrey
                    font.pixelSize: 12
                    font.family: Theme.fontFamily
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: (typeof localeController !== "undefined" && localeController)
                              ? localeController.copyright : "Copyright \u00a9 2026 Aegra"
                        color: Theme.colorTextDim
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                    }

                    Item { Layout.fillWidth: true }

                    AppButton {
                        text: qsTrId("aegra.about.licenses_btn")
                        implicitHeight: 26
                        leftPadding: 10
                        rightPadding: 10
                        onClicked: {
                            root.licensesRequested()
                        }
                    }
                }
            }
        }

        // Footer
        RowLayout {
            Layout.fillWidth: true

            Item { Layout.fillWidth: true }

            AppButton {
                text: qsTrId("aegra.common.ok")
                primary: true
                implicitHeight: 30
                leftPadding: 20
                rightPadding: 20
                onClicked: root.close()
            }
        }
    }
}
