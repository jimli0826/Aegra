import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

// Redesigned HomePage 1:1 aligned with CoachPro mockup with spring-bounce entrance animations
Item {
    id: root
    //% "Home"
    Accessible.name: qsTrId("aegra.nav.home")
    signal homeNavigate(int index)

    // Staggered Entrance Animation States
    property bool animStage1: false
    property bool animStage2: false
    property bool animStage3: false
    property bool animStage4: false
    property bool animStage5: false

    function restartEntranceAnimation() {
        animStage1 = false
        animStage2 = false
        animStage3 = false
        animStage4 = false
        animStage5 = false
        t1.restart()
    }

    Timer { id: t1; interval: 50;  repeat: false; onTriggered: root.animStage1 = true }
    Timer { id: t2; interval: 130; repeat: false; onTriggered: root.animStage2 = true }
    Timer { id: t3; interval: 210; repeat: false; onTriggered: root.animStage3 = true }
    Timer { id: t4; interval: 310; repeat: false; onTriggered: root.animStage4 = true }
    Timer { id: t5; interval: 430; repeat: false; onTriggered: root.animStage5 = true }

    onAnimStage1Changed: if (animStage1) t2.restart()
    onAnimStage2Changed: if (animStage2) t3.restart()
    onAnimStage3Changed: if (animStage3) t4.restart()
    onAnimStage4Changed: if (animStage4) t5.restart()

    Component.onCompleted: restartEntranceAnimation()
    onVisibleChanged: {
        if (visible)
            restartEntranceAnimation()
    }

    // Standings Table Data (matching CoachPro mockup table)
    readonly property var standingsItems: [
        { rank: "1", name: "Juventus", type: "系统与数据卷", mp: "8", w: "6", d: "1", l: "1", g: "13:5", pts: "19", iconType: "juventus" },
        { rank: "2", name: "Atalanta", type: "数据 NAS 卷", mp: "8", w: "5", d: "1", l: "3", g: "10:2", pts: "16", iconType: "atalanta" },
        { rank: "3", name: "Inter", type: "移动便携卷", mp: "8", w: "5", d: "0", l: "3", g: "10:3", pts: "15", iconType: "inter" },
        { rank: "4", name: "Napoli", type: "云端异地冷备", mp: "8", w: "4", d: "1", l: "3", g: "14:6", pts: "13", iconType: "napoli" },
        { rank: "5", name: "Milan", type: "磁带归档库", mp: "8", w: "4", d: "1", l: "3", g: "8:4", pts: "13", iconType: "milan" },
        { rank: "6", name: "Roma", type: "灾备节点卷", mp: "8", w: "4", d: "0", l: "4", g: "7:3", pts: "12", iconType: "roma" }
    ]

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: mainCol.implicitHeight + 40
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: mainCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 24
            spacing: 20


            // ===============================================
            // MAIN 2-COLUMN DASHBOARD GRID
            // ===============================================
            RowLayout {
                Layout.fillWidth: true
                spacing: 20
                Layout.alignment: Qt.AlignTop

                // ===============================================
                // LEFT COLUMN (~1.15 Ratio)
                // ===============================================
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 540
                    spacing: 20
                    Layout.alignment: Qt.AlignTop

                    // Card 1: Next Backup ("下一次计划备份")
                    Card {
                        id: card1
                        Layout.fillWidth: true
                        implicitHeight: 180
                        title: "下一次计划备份"
                        actionText: "管理备份计划"
                        onActionClicked: root.homeNavigate(1)

                        opacity: root.animStage1 ? 1 : 0
                        transform: Translate { y: root.animStage1 ? 0 : 36 }
                        scale: root.animStage1 ? 1.0 : 0.95
                        Behavior on opacity { NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }
                        Behavior on transform { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
                        Behavior on scale { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

                        ColumnLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 52
                            anchors.leftMargin: 22
                            anchors.rightMargin: 22
                            spacing: 14

                            // Subtitle Badge & Time
                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 10
                                Rectangle {
                                    width: 130
                                    height: 24
                                    radius: 12
                                    color: "#e2f2ef"
                                    Text {
                                        anchors.centerIn: parent
                                        text: "🛡️ 系统与数据卷增量"
                                        color: "#2A7982"
                                        font.pixelSize: 11
                                        font.bold: true
                                        font.family: Theme.fontFamily
                                    }
                                }
                                Text {
                                    text: "今晚 21:00 · 目标: Local Vault"
                                    color: "#7A9190"
                                    font.pixelSize: 12
                                    font.family: Theme.fontFamily
                                    Layout.alignment: Qt.AlignVCenter
                                }
                            }

                            // Match / Sync Display (Source -> SYNC -> Target)
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 16

                                Item { Layout.fillWidth: true }

                                // Source Box
                                Rectangle {
                                    width: 170
                                    height: 52
                                    radius: 14
                                    color: "#F7FAF9"
                                    Row {
                                        anchors.centerIn: parent
                                        spacing: 10
                                        Text { text: "💾"; font.pixelSize: 20; anchors.verticalCenter: parent.verticalCenter }
                                        Text { text: "C: & D: 盘"; color: "#111111"; font.pixelSize: 14; font.bold: true; font.family: Theme.fontFamily; anchors.verticalCenter: parent.verticalCenter }
                                    }
                                }

                                // SYNC Badge
                                Rectangle {
                                    width: 38
                                    height: 38
                                    radius: 19
                                    scale: root.animStage1 ? 1.0 : 0.3
                                    Behavior on scale { NumberAnimation { duration: 500; easing.type: Easing.OutBack; easing.overshoot: 1.5 } }
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "#FF7B8B" }
                                        GradientStop { position: 1.0; color: "#EE6476" }
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        text: "SYNC"
                                        color: "#ffffff"
                                        font.pixelSize: 10
                                        font.bold: true
                                        font.family: Theme.fontFamily
                                    }
                                }

                                // Target Box
                                Rectangle {
                                    width: 170
                                    height: 52
                                    radius: 14
                                    color: "#F7FAF9"
                                    Row {
                                        anchors.centerIn: parent
                                        spacing: 10
                                        Text { text: "🗄️"; font.pixelSize: 20; anchors.verticalCenter: parent.verticalCenter }
                                        Text { text: "Local Vault (E:)"; color: "#111111"; font.pixelSize: 14; font.bold: true; font.family: Theme.fontFamily; anchors.verticalCenter: parent.verticalCenter }
                                    }
                                }

                                Item { Layout.fillWidth: true }
                            }
                        }
                    }

                    // Card 2: Standings Repositories Table ("受保护存储仓库")
                    Card {
                        id: card2
                        Layout.fillWidth: true
                        implicitHeight: 390
                        title: "受保护存储仓库 (Standings)"
                        actionText: "View all"
                        onActionClicked: root.homeNavigate(4)

                        opacity: root.animStage2 ? 1 : 0
                        transform: Translate { y: root.animStage2 ? 0 : 36 }
                        scale: root.animStage2 ? 1.0 : 0.95
                        Behavior on opacity { NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }
                        Behavior on transform { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
                        Behavior on scale { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

                        ColumnLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 52
                            anchors.leftMargin: 20
                            anchors.rightMargin: 20
                            spacing: 6

                            // Table Header
                            RowLayout {
                                Layout.fillWidth: true
                                height: 26
                                Text { text: "#"; color: "#7A9190"; font.pixelSize: 11; font.bold: true; Layout.preferredWidth: 24 }
                                Text { text: "VAULT REPOSITORY"; color: "#7A9190"; font.pixelSize: 11; font.bold: true; Layout.fillWidth: true }
                                Text { text: "MP"; color: "#7A9190"; font.pixelSize: 11; font.bold: true; Layout.preferredWidth: 32; horizontalAlignment: Text.AlignHCenter }
                                Text { text: "W"; color: "#7A9190"; font.pixelSize: 11; font.bold: true; Layout.preferredWidth: 32; horizontalAlignment: Text.AlignHCenter }
                                Text { text: "D"; color: "#7A9190"; font.pixelSize: 11; font.bold: true; Layout.preferredWidth: 32; horizontalAlignment: Text.AlignHCenter }
                                Text { text: "L"; color: "#7A9190"; font.pixelSize: 11; font.bold: true; Layout.preferredWidth: 32; horizontalAlignment: Text.AlignHCenter }
                                Text { text: "G"; color: "#7A9190"; font.pixelSize: 11; font.bold: true; Layout.preferredWidth: 48; horizontalAlignment: Text.AlignHCenter }
                                Text { text: "PTS"; color: "#7A9190"; font.pixelSize: 11; font.bold: true; Layout.preferredWidth: 35; horizontalAlignment: Text.AlignRight }
                            }

                            // Table Rows matching CoachPro mockup with staggered entrance
                            Repeater {
                                model: root.standingsItems
                                delegate: Rectangle {
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    height: 48
                                    radius: 10
                                    color: rowMouse.containsMouse ? "#F7FAF9" : "transparent"
                                    Behavior on color { ColorAnimation { duration: 150 } }

                                    opacity: root.animStage2 ? 1 : 0
                                    transform: Translate { x: root.animStage2 ? 0 : -16 }
                                    Behavior on opacity { NumberAnimation { duration: 320 + index * 45; easing.type: Easing.OutCubic } }
                                    Behavior on transform { NumberAnimation { duration: 400 + index * 45; easing.type: Easing.OutBack; easing.overshoot: 1.2 } }

                                    MouseArea {
                                        id: rowMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 4
                                        anchors.rightMargin: 4

                                        Text { text: modelData.rank; color: "#7A9190"; font.pixelSize: 13; font.bold: true; Layout.preferredWidth: 24 }

                                        Row {
                                            Layout.fillWidth: true
                                            spacing: 12
                                            Rectangle {
                                                width: 26
                                                height: 26
                                                radius: 8
                                                color: "#ffffff"
                                                border.width: 1
                                                border.color: "#ebf3f2"
                                                anchors.verticalCenter: parent.verticalCenter
                                                
                                                Rectangle {
                                                    anchors.centerIn: parent
                                                    width: 10; height: 10; radius: 5
                                                    color: modelData.iconType === "juventus" ? "#111111" : (modelData.iconType === "atalanta" ? "#2A7982" : (modelData.iconType === "inter" ? "#1E5C64" : (modelData.iconType === "napoli" ? "#0080FF" : (modelData.iconType === "milan" ? "#EE6476" : "#FF8F00"))))
                                                }
                                            }
                                            Text {
                                                text: modelData.name
                                                color: "#111111"
                                                font.pixelSize: 13
                                                font.bold: true
                                                font.family: Theme.fontFamily
                                                anchors.verticalCenter: parent.children[0].verticalCenter
                                            }
                                        }

                                        Text { text: modelData.mp; color: "#7A9190"; font.pixelSize: 12; font.bold: true; Layout.preferredWidth: 32; horizontalAlignment: Text.AlignHCenter }
                                        Text { text: modelData.w; color: "#7A9190"; font.pixelSize: 12; font.bold: true; Layout.preferredWidth: 32; horizontalAlignment: Text.AlignHCenter }
                                        Text { text: modelData.d; color: "#7A9190"; font.pixelSize: 12; font.bold: true; Layout.preferredWidth: 32; horizontalAlignment: Text.AlignHCenter }
                                        Text { text: modelData.l; color: "#7A9190"; font.pixelSize: 12; font.bold: true; Layout.preferredWidth: 32; horizontalAlignment: Text.AlignHCenter }
                                        Text { text: modelData.g; color: "#7A9190"; font.pixelSize: 12; font.bold: true; Layout.preferredWidth: 48; horizontalAlignment: Text.AlignHCenter }
                                        Text { text: modelData.pts; color: "#111111"; font.pixelSize: 14; font.bold: true; Layout.preferredWidth: 35; horizontalAlignment: Text.AlignRight }
                                    }
                                }
                            }
                        }
                    }
                }

                // ===============================================
                // RIGHT COLUMN (~0.95 Ratio)
                // ===============================================
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 440
                    spacing: 20
                    Layout.alignment: Qt.AlignTop

                    // Card 3: Task Health Statistics ("任务健康状态")
                    Card {
                        id: card3
                        Layout.fillWidth: true
                        implicitHeight: 180
                        title: "任务健康状态"
                        actionText: "View all statistic"
                        onActionClicked: root.homeNavigate(5)

                        opacity: root.animStage3 ? 1 : 0
                        transform: Translate { y: root.animStage3 ? 0 : 36 }
                        scale: root.animStage3 ? 1.0 : 0.95
                        Behavior on opacity { NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }
                        Behavior on transform { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
                        Behavior on scale { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

                        ColumnLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 52
                            anchors.leftMargin: 22
                            anchors.rightMargin: 22
                            spacing: 20

                            // Segmented Bar Graph with bouncy spring width growth
                            Rectangle {
                                id: barContainer
                                Layout.fillWidth: true
                                height: 10
                                radius: 5
                                color: "#EBF3F2"
                                clip: true

                                Row {
                                    anchors.fill: parent
                                    Rectangle {
                                        width: root.animStage3 ? parent.width * 0.72 : 0
                                        height: parent.height
                                        color: "#2A7982"
                                        Behavior on width { NumberAnimation { duration: 900; easing.type: Easing.OutBack; easing.overshoot: 1.15 } }
                                    }
                                    Rectangle {
                                        width: root.animStage3 ? parent.width * 0.18 : 0
                                        height: parent.height
                                        color: "#84CBB8"
                                        Behavior on width { NumberAnimation { duration: 900; easing.type: Easing.OutBack; easing.overshoot: 1.15 } }
                                    }
                                    Rectangle {
                                        width: root.animStage3 ? parent.width * 0.10 : 0
                                        height: parent.height
                                        color: "#EE6877"
                                        Behavior on width { NumberAnimation { duration: 900; easing.type: Easing.OutBack; easing.overshoot: 1.15 } }
                                    }
                                }
                            }

                            // 4 Status Numbers
                            RowLayout {
                                Layout.fillWidth: true

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4
                                    Text { text: "TOTAL"; color: "#7A9190"; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; Layout.alignment: Qt.AlignHCenter }
                                    Text { text: "28"; color: "#111111"; font.pixelSize: 24; font.bold: true; font.family: Theme.fontFamily; horizontalAlignment: Text.AlignHCenter; Layout.alignment: Qt.AlignHCenter }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4
                                    Text { text: "SUCCESS"; color: "#7A9190"; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; Layout.alignment: Qt.AlignHCenter }
                                    Text { text: "25"; color: "#111111"; font.pixelSize: 24; font.bold: true; font.family: Theme.fontFamily; horizontalAlignment: Text.AlignHCenter; Layout.alignment: Qt.AlignHCenter }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4
                                    Text { text: "RUNNING"; color: "#7A9190"; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; Layout.alignment: Qt.AlignHCenter }
                                    Text { text: "2"; color: "#111111"; font.pixelSize: 24; font.bold: true; font.family: Theme.fontFamily; horizontalAlignment: Text.AlignHCenter; Layout.alignment: Qt.AlignHCenter }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4
                                    Text { text: "WARNING"; color: "#7A9190"; font.pixelSize: 10; font.bold: true; horizontalAlignment: Text.AlignHCenter; Layout.alignment: Qt.AlignHCenter }
                                    Text { text: "1"; color: "#111111"; font.pixelSize: 24; font.bold: true; font.family: Theme.fontFamily; horizontalAlignment: Text.AlignHCenter; Layout.alignment: Qt.AlignHCenter }
                                }
                            }
                        }
                    }

                    // Card 4: 2x2 Stat Metric Cards Grid
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 14
                        rowSpacing: 14

                        // Metric 1: Protected Data
                        Card {
                            Layout.fillWidth: true
                            implicitHeight: 90

                            opacity: root.animStage4 ? 1 : 0
                            transform: Translate { y: root.animStage4 ? 0 : 36 }
                            scale: root.animStage4 ? 1.0 : 0.95
                            Behavior on opacity { NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }
                            Behavior on transform { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
                            Behavior on scale { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 14

                                Rectangle {
                                    id: iconBox1
                                    width: 44
                                    height: 44
                                    radius: 14
                                    transformOrigin: Item.Center
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "#9A73FF" }
                                        GradientStop { position: 1.0; color: "#7C4DFF" }
                                    }
                                    Text { anchors.centerIn: parent; text: "⏱️"; font.pixelSize: 20 }

                                    ParallelAnimation {
                                        running: root.animStage4
                                        NumberAnimation {
                                            target: iconBox1
                                            property: "scale"
                                            from: 0.2
                                            to: 1.0
                                            duration: 650
                                            easing.type: Easing.OutBack
                                            easing.overshoot: 1.8
                                        }
                                        NumberAnimation {
                                            target: iconBox1
                                            property: "rotation"
                                            from: -25
                                            to: 0
                                            duration: 650
                                            easing.type: Easing.OutBack
                                            easing.overshoot: 1.8
                                        }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 2
                                    Text { text: "已保护数据量"; color: "#7A9190"; font.pixelSize: 10; font.bold: true }
                                    Text { text: "1.84 TB"; color: "#111111"; font.pixelSize: 20; font.bold: true; font.family: Theme.fontFamily }
                                }
                            }
                        }

                        // Metric 2: Storage Used
                        Card {
                            Layout.fillWidth: true
                            implicitHeight: 90

                            opacity: root.animStage4 ? 1 : 0
                            transform: Translate { y: root.animStage4 ? 0 : 36 }
                            scale: root.animStage4 ? 1.0 : 0.95
                            Behavior on opacity { NumberAnimation { duration: 400; easing.type: Easing.OutCubic } }
                            Behavior on transform { NumberAnimation { duration: 540; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
                            Behavior on scale { NumberAnimation { duration: 540; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 14

                                Rectangle {
                                    id: iconBox2
                                    width: 44
                                    height: 44
                                    radius: 14
                                    transformOrigin: Item.Center
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "#FF6BB0" }
                                        GradientStop { position: 1.0; color: "#E91E63" }
                                    }
                                    Text { anchors.centerIn: parent; text: "💲"; font.pixelSize: 20 }

                                    ParallelAnimation {
                                        running: root.animStage4
                                        NumberAnimation {
                                            target: iconBox2
                                            property: "scale"
                                            from: 0.2
                                            to: 1.0
                                            duration: 680
                                            easing.type: Easing.OutBack
                                            easing.overshoot: 1.8
                                        }
                                        NumberAnimation {
                                            target: iconBox2
                                            property: "rotation"
                                            from: -25
                                            to: 0
                                            duration: 680
                                            easing.type: Easing.OutBack
                                            easing.overshoot: 1.8
                                        }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 2
                                    Text { text: "存储总占用"; color: "#7A9190"; font.pixelSize: 10; font.bold: true }
                                    Text { text: "690 GB"; color: "#111111"; font.pixelSize: 20; font.bold: true; font.family: Theme.fontFamily }
                                }
                            }
                        }

                        // Metric 3: Dedupe Ratio
                        Card {
                            Layout.fillWidth: true
                            implicitHeight: 90

                            opacity: root.animStage4 ? 1 : 0
                            transform: Translate { y: root.animStage4 ? 0 : 36 }
                            scale: root.animStage4 ? 1.0 : 0.95
                            Behavior on opacity { NumberAnimation { duration: 420; easing.type: Easing.OutCubic } }
                            Behavior on transform { NumberAnimation { duration: 560; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
                            Behavior on scale { NumberAnimation { duration: 560; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 14

                                Rectangle {
                                    id: iconBox3
                                    width: 44
                                    height: 44
                                    radius: 14
                                    transformOrigin: Item.Center
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "#FFA726" }
                                        GradientStop { position: 1.0; color: "#FB8C00" }
                                    }
                                    Text { anchors.centerIn: parent; text: "📊"; font.pixelSize: 20 }

                                    ParallelAnimation {
                                        running: root.animStage4
                                        NumberAnimation {
                                            target: iconBox3
                                            property: "scale"
                                            from: 0.2
                                            to: 1.0
                                            duration: 710
                                            easing.type: Easing.OutBack
                                            easing.overshoot: 1.8
                                        }
                                        NumberAnimation {
                                            target: iconBox3
                                            property: "rotation"
                                            from: -25
                                            to: 0
                                            duration: 710
                                            easing.type: Easing.OutBack
                                            easing.overshoot: 1.8
                                        }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 2
                                    Text { text: "重删与压缩率"; color: "#7A9190"; font.pixelSize: 10; font.bold: true }
                                    Text { text: "2.67 : 1"; color: "#111111"; font.pixelSize: 20; font.bold: true; font.family: Theme.fontFamily }
                                }
                            }
                        }

                        // Metric 4: Transfer Speed
                        Card {
                            Layout.fillWidth: true
                            implicitHeight: 90

                            opacity: root.animStage4 ? 1 : 0
                            transform: Translate { y: root.animStage4 ? 0 : 36 }
                            scale: root.animStage4 ? 1.0 : 0.95
                            Behavior on opacity { NumberAnimation { duration: 440; easing.type: Easing.OutCubic } }
                            Behavior on transform { NumberAnimation { duration: 580; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
                            Behavior on scale { NumberAnimation { duration: 580; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 14

                                Rectangle {
                                    id: iconBox4
                                    width: 44
                                    height: 44
                                    radius: 14
                                    transformOrigin: Item.Center
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "#54E0B7" }
                                        GradientStop { position: 1.0; color: "#00BFA5" }
                                    }
                                    Text { anchors.centerIn: parent; text: "⭐"; font.pixelSize: 20 }

                                    ParallelAnimation {
                                        running: root.animStage4
                                        NumberAnimation {
                                            target: iconBox4
                                            property: "scale"
                                            from: 0.2
                                            to: 1.0
                                            duration: 740
                                            easing.type: Easing.OutBack
                                            easing.overshoot: 1.8
                                        }
                                        NumberAnimation {
                                            target: iconBox4
                                            property: "rotation"
                                            from: -25
                                            to: 0
                                            duration: 740
                                            easing.type: Easing.OutBack
                                            easing.overshoot: 1.8
                                        }
                                    }
                                }

                                ColumnLayout {
                                    spacing: 2
                                    Text { text: "平均传输速度"; color: "#7A9190"; font.pixelSize: 10; font.bold: true }
                                    Text { text: "248 MB/s"; color: "#111111"; font.pixelSize: 20; font.bold: true; font.family: Theme.fontFamily }
                                }
                            }
                        }
                    }

                    // Card 5: Action Banner Card ("数据安全提醒")
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 140
                        radius: 20
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: Theme.colorMenuActive }
                            GradientStop { position: 1.0; color: Theme.colorMenuActiveEnd }
                        }

                        opacity: root.animStage5 ? 1 : 0
                        transform: Translate { y: root.animStage5 ? 0 : 36 }
                        scale: root.animStage5 ? 1.0 : 0.95
                        Behavior on opacity { NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }
                        Behavior on transform { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }
                        Behavior on scale { NumberAnimation { duration: 520; easing.type: Easing.OutBack; easing.overshoot: 1.25 } }

                        ColumnLayout {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.margins: 22
                            spacing: 8
                            Layout.alignment: Qt.AlignVCenter

                            Text {
                                text: "数据安全提醒"
                                color: "#A5C7F9"
                                font.pixelSize: 10
                                font.bold: true
                                font.letterSpacing: 0.8
                            }
                            Text {
                                text: "立即为重要卷配置异地冷备计划"
                                color: "#ffffff"
                                font.pixelSize: 18
                                font.bold: true
                                font.family: Theme.fontFamily
                            }
                            Rectangle {
                                width: 136
                                height: 32
                                radius: 16
                                color: bannerBtnMouse.containsMouse ? "#F0FAF8" : "#ffffff"
                                Behavior on color { ColorAnimation { duration: 150 } }

                                Text {
                                    anchors.centerIn: parent
                                    text: "前往新建备份计划"
                                    color: Theme.colorMenuActiveEnd
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.family: Theme.fontFamily
                                }
                                MouseArea {
                                    id: bannerBtnMouse
                                    anchors.fill: parent
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
