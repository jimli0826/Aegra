import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import "."
import "components"
import "pages"

Window {
    id: window
    visible: true
    width: 1080
    height: 720
    minimumWidth: 900
    minimumHeight: 600
    //% "Aegra"
    title: qsTrId("aegra.app.title")
    color: Theme.colorBg
    flags: Qt.Window | Qt.FramelessWindowHint

    readonly property bool canResize: visibility !== Window.Maximized
                                      && visibility !== Window.FullScreen

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            color: Theme.colorHeader

            MouseArea {
                anchors.fill: parent
                anchors.topMargin: window.canResize ? 6 : 0
                onPressed: window.startSystemMove()
                onDoubleClicked: {
                    if (window.visibility === Window.Maximized)
                        window.showNormal()
                    else
                        window.showMaximized()
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                spacing: 9

                Image {
                    source: "qrc:/Aegra/icons/product_32.png"
                    sourceSize.width: 32
                    sourceSize.height: 32
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }

                Text {
                    //% "Aegra"
                    text: qsTrId("aegra.app.title")
                    color: Theme.colorTextWhite
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.bold: true
                }

                Text {
                    text: serviceClient.serviceVersion.length > 0
                          ? "V" + serviceClient.serviceVersion : ""
                    color: Theme.colorTextDim
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    Layout.preferredWidth: 7
                    Layout.preferredHeight: 7
                    radius: 4
                    color: serviceClient.connected ? Theme.colorGreen : Theme.colorAccentRed
                }

                Text {
                    //% "Service %1"
                    text: qsTrId("aegra.shell.service_label").arg(serviceClient.statusText)
                    color: Theme.colorTextGrey
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    Layout.rightMargin: 4
                }

                ComboBox {
                    id: languageCombo
                    Layout.preferredHeight: 24
                    Layout.preferredWidth: 118
                    Layout.rightMargin: 8
                    model: localeController.availableLanguages
                    textRole: "label"
                    currentIndex: {
                        const tags = localeController.availableLanguages
                        for (let i = 0; i < tags.length; ++i) {
                            if (tags[i].tag === localeController.language)
                                return i
                        }
                        return 0
                    }
                    onActivated: function(index) {
                        const languages = localeController.availableLanguages
                        if (index >= 0 && index < languages.length)
                            localeController.setLanguage(languages[index].tag)
                    }

                    background: Rectangle {
                        color: languageCombo.hovered ? Theme.colorButtonHover : Theme.colorButton
                        border.width: 1
                        border.color: Theme.colorBorder
                        radius: 3
                    }
                    contentItem: Text {
                        leftPadding: 8
                        rightPadding: 18
                        text: languageCombo.displayText
                        color: Theme.colorTextWhite
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    popup: Popup {
                        y: languageCombo.height
                        width: languageCombo.width
                        implicitHeight: contentItem.implicitHeight
                        padding: 1
                        contentItem: ListView {
                            clip: true
                            implicitHeight: contentHeight
                            model: languageCombo.popup.visible ? languageCombo.delegateModel : null
                            currentIndex: languageCombo.highlightedIndex
                        }
                        background: Rectangle {
                            color: Theme.colorCard
                            border.color: Theme.colorBorder
                            radius: 3
                        }
                    }
                    delegate: ItemDelegate {
                        width: languageCombo.width
                        height: 28
                        contentItem: Text {
                            text: modelData.label
                            color: Theme.colorTextWhite
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: parent.highlighted ? Theme.colorHover : "transparent"
                        }
                    }
                }

                Row {
                    spacing: 0

                    WindowButton {
                        icon: "\uE921"
                        onClicked: window.showMinimized()
                    }
                    WindowButton {
                        icon: window.visibility === Window.Maximized ? "\uE923" : "\uE922"
                        onClicked: window.visibility === Window.Maximized
                                   ? window.showNormal() : window.showMaximized()
                    }
                    WindowButton {
                        icon: "\uE8BB"
                        isClose: true
                        onClicked: window.close()
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            SidebarMenu {
                id: sideMenu
                Layout.preferredWidth: sideWidth
                Layout.minimumWidth: sideWidth
                Layout.maximumWidth: sideWidth
                Layout.fillHeight: true
                currentIndex: 4
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.colorBg

                RepositoryPage {
                    anchors.fill: parent
                }
            }
        }
    }

    ResizeHandle {
        targetWindow: window
        edges: Qt.TopEdge
        resizeCursor: Qt.SizeVerCursor
        visible: window.canResize
        height: 6
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
    }
    ResizeHandle {
        targetWindow: window
        edges: Qt.BottomEdge
        resizeCursor: Qt.SizeVerCursor
        visible: window.canResize
        height: 6
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
    ResizeHandle {
        targetWindow: window
        edges: Qt.LeftEdge
        resizeCursor: Qt.SizeHorCursor
        visible: window.canResize
        width: 6
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
    }
    ResizeHandle {
        targetWindow: window
        edges: Qt.RightEdge
        resizeCursor: Qt.SizeHorCursor
        visible: window.canResize
        width: 6
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
    }
    ResizeHandle {
        targetWindow: window
        edges: Qt.TopEdge | Qt.LeftEdge
        resizeCursor: Qt.SizeFDiagCursor
        visible: window.canResize
        width: 10
        height: 10
        anchors.left: parent.left
        anchors.top: parent.top
    }
    ResizeHandle {
        targetWindow: window
        edges: Qt.TopEdge | Qt.RightEdge
        resizeCursor: Qt.SizeBDiagCursor
        visible: window.canResize
        width: 10
        height: 10
        anchors.right: parent.right
        anchors.top: parent.top
    }
    ResizeHandle {
        targetWindow: window
        edges: Qt.BottomEdge | Qt.LeftEdge
        resizeCursor: Qt.SizeBDiagCursor
        visible: window.canResize
        width: 10
        height: 10
        anchors.left: parent.left
        anchors.bottom: parent.bottom
    }
    ResizeHandle {
        targetWindow: window
        edges: Qt.BottomEdge | Qt.RightEdge
        resizeCursor: Qt.SizeFDiagCursor
        visible: window.canResize
        width: 10
        height: 10
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
