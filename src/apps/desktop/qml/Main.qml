import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import "."
import "components"
import "pages"

// Shell chrome aligned with backup/src/gui Main.qml (title bar, sidebar, page fade).
// Starts as compact splash window (600×440), then expands to main UI (1080×720).
Window {
    id: window
    // Start as splash window only (compact card; not full main UI frame)
    width: 600
    height: 440
    visible: true
    //% "Aegra"
    title: qsTrId("aegra.app.title")
    color: Theme.colorCard
    flags: Qt.Window | Qt.FramelessWindowHint
    minimumWidth: appReady ? 900 : 400
    minimumHeight: appReady ? 600 : 300

    readonly property int mainWidth: 1080
    readonly property int mainHeight: 720
    readonly property int resizeBorder: 6
    readonly property bool canResize: appReady
                                      && visibility !== Window.Maximized
                                      && visibility !== Window.FullScreen
    /// Settings opens as right drawer (old Main.qml pattern)
    property bool settingsPanelOpen: false
    /// Latched once splash dismisses so main opacity/animation never re-toggles.
    property bool appReady: false
    /// Global busy after splash: page catalog reload / commands (old Main.qml appLoading).
    /// Bound to each loading flag so NOTIFY from domain signals is enough (not only loadingChanged).
    readonly property bool appLoading: {
        if (!window.appReady || !serviceClient.connected)
            return false
        return serviceClient.jobsLoading
                || serviceClient.inventoryLoading
                || serviceClient.connectionsLoading
                || serviceClient.schedulesLoading
                || serviceClient.repositoryLoading
                || serviceClient.repositoryCommandBusy
                || serviceClient.backupCommandBusy
                || serviceClient.cancelCommandBusy
    }

    function centerOnScreen() {
        var scr = window.screen
        if (!scr)
            return
        window.x = scr.virtualX + Math.round((scr.width - window.width) / 2)
        window.y = scr.virtualY + Math.round((scr.height - window.height) / 2)
    }

    function applySplashSize() {
        if (window.appReady)
            return
        var w = splash.preferredWidth || 600
        var h = splash.preferredHeight || 440
        window.width = w
        window.height = h
        window.color = Theme.colorCard
        centerOnScreen()
    }

    function applyMainSize() {
        window.width = window.mainWidth
        window.height = window.mainHeight
        window.color = Theme.colorBg
        centerOnScreen()
    }

    function enterMainUi() {
        if (window.appReady)
            return
        window.appReady = true
        splash.windowAppReady = true
        applyMainSize()
    }

    // Keep window chrome in sync when theme changes (old Main.qml pattern)
    Connections {
        target: Theme
        function onThemeIdChanged() {
            window.color = window.appReady ? Theme.colorBg : Theme.colorCard
        }
        function onColorBgChanged() {
            if (window.appReady)
                window.color = Theme.colorBg
        }
        function onColorCardChanged() {
            if (!window.appReady)
                window.color = Theme.colorCard
        }
    }

    Connections {
        target: serviceClient
        function onSplashChanged() {
            if (!serviceClient.splashVisible)
                window.enterMainUi()
            else if (!window.appReady)
                window.applySplashSize()
        }
    }

    Component.onCompleted: {
        // Load persisted theme after context properties exist (old Theme.initFromBackend)
        Theme.initFromBackend()
        if (!serviceClient.splashVisible) {
            window.enterMainUi()
        } else {
            window.applySplashSize()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        // Hidden under splash; fade in once when ready (never re-hide → no flicker).
        opacity: window.appReady ? 1 : 0
        enabled: window.appReady
        Behavior on opacity {
            enabled: window.appReady
            NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
        }

        // ========== Title Bar ==========
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            color: Theme.colorHeader

            MouseArea {
                anchors.fill: parent
                anchors.topMargin: window.canResize ? window.resizeBorder : 0
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
                anchors.rightMargin: 8
                spacing: 10

                Image {
                    source: "qrc:/Aegra/icons/product_32.png"
                    sourceSize.width: 32
                    sourceSize.height: 32
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    Layout.alignment: Qt.AlignVCenter
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    mipmap: true
                }

                Text {
                    //% "Aegra"
                    text: qsTrId("aegra.app.title")
                    color: Theme.colorTextWhite
                    font.pixelSize: 12
                    font.family: Theme.fontFamily
                    font.bold: true
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    text: serviceClient.serviceVersion.length > 0
                          ? "V" + serviceClient.serviceVersion : ""
                    color: Theme.colorTextDim
                    font.pixelSize: 11
                    font.family: Theme.fontFamily
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 2
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    Layout.preferredWidth: 7
                    Layout.preferredHeight: 7
                    radius: 4
                    Layout.alignment: Qt.AlignVCenter
                    color: serviceClient.connected ? Theme.colorGreen
                           : (serviceClient.statusText.indexOf("Connect") >= 0
                              ? Theme.colorAccentBlue : Theme.colorAccentRed)
                }
                Text {
                    //% "Service %1"
                    text: qsTrId("aegra.shell.service_label").arg(serviceClient.statusText)
                    color: serviceClient.connected ? Theme.colorTextGrey
                           : (statusHover.containsMouse ? Theme.colorAccentBlue
                                                        : Theme.colorTextGrey)
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    Layout.alignment: Qt.AlignVCenter
                    MouseArea {
                        id: statusHover
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        cursorShape: serviceClient.connected ? Qt.ArrowCursor
                                                             : Qt.PointingHandCursor
                        enabled: !serviceClient.connected
                        onClicked: serviceClient.reconnect()
                    }
                }
                Text {
                    visible: serviceClient.jobListAvailable && serviceClient.jobs.activeCount > 0
                    //% "Tasks %1"
                    text: qsTrId("aegra.shell.tasks_badge").arg(serviceClient.jobs.activeCount)
                    color: Theme.colorTextGrey
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    Layout.alignment: Qt.AlignVCenter
                    Layout.rightMargin: 4
                }

                // Language switch lives in Settings only (not title bar).
                Row {
                    spacing: 0
                    Layout.alignment: Qt.AlignVCenter
                    WindowButton {
                        icon: "\u2500"
                        onClicked: window.showMinimized()
                    }
                    WindowButton {
                        icon: window.visibility === Window.Maximized ? "\u2750" : "\u25A1"
                        onClicked: window.visibility === Window.Maximized
                                   ? window.showNormal() : window.showMaximized()
                    }
                    WindowButton {
                        icon: "\u2715"
                        isClose: true
                        onClicked: window.close()
                    }
                }
            }
        }

        // ========== Main Content ==========
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            SidebarMenu {
                id: sideMenu
                Layout.preferredWidth: sideMenu.sideWidth
                Layout.minimumWidth: sideMenu.sideWidth
                Layout.maximumWidth: sideMenu.sideWidth
                Layout.fillHeight: true
                currentIndex: pageContainer.currentIndex
                settingsActive: window.settingsPanelOpen
                homeEnabled: true
                backupEnabled: true
                restoreEnabled: true
                mountEnabled: true
                repositoryEnabled: true
                eventLogEnabled: true
                settingsEnabled: true
                onMenuClicked: function(index) {
                    window.settingsPanelOpen = false
                    pageContainer.switchPage(index)
                }
                onSettingsClicked: {
                    window.settingsPanelOpen = !window.settingsPanelOpen
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.colorBg

                Item {
                    id: pageContainer
                    anchors.fill: parent
                    clip: true
                    property int currentIndex: 0
                    property int previousIndex: 0

                    function switchPage(newIndex) {
                        if (newIndex === currentIndex)
                            return
                        if (newIndex < 0 || newIndex > 5)
                            return
                        previousIndex = currentIndex
                        currentIndex = newIndex
                        sideMenu.currentIndex = newIndex
                        // Reload page data on every menu switch (old onPageActivated / refresh*).
                        // LoadingOverlay covers UI while ServiceClient.globalLoading is true.
                        if (newIndex === 0) {
                            serviceClient.refreshJobs()
                        } else if (newIndex === 1) {
                            serviceClient.refreshInventory()
                            serviceClient.refreshConnections()
                            serviceClient.refreshSchedules()
                        } else if (newIndex === 2 || newIndex === 3) {
                            // Restore/Mount target list uses source inventory disksTree.
                            serviceClient.refreshInventory()
                            serviceClient.refreshConnections()
                            serviceClient.refreshRepository()
                        } else if (newIndex === 4) {
                            serviceClient.refreshConnections()
                            serviceClient.refreshRepository()
                        }
                        // Event Log: no Service catalog query yet.
                    }

                    // 0 Home, 1 Backup, 2 Restore, 3 Mount, 4 Repository, 5 Event Log
                    Repeater {
                        model: 6
                        Loader {
                            anchors.fill: parent
                            opacity: index === pageContainer.currentIndex ? 1 : 0
                            visible: opacity > 0
                            scale: index === pageContainer.currentIndex ? 1 : 0.98
                            active: index === pageContainer.currentIndex
                                    || index === pageContainer.previousIndex

                            Behavior on opacity {
                                NumberAnimation { duration: 320; easing.type: Easing.OutCubic }
                            }
                            Behavior on scale {
                                NumberAnimation { duration: 320; easing.type: Easing.OutCubic }
                            }

                            sourceComponent: {
                                switch (index) {
                                case 0: return homePageComp
                                case 1: return backupPageComp
                                case 2: return restorePageComp
                                case 3: return mountPageComp
                                case 4: return repositoryPageComp
                                case 5: return eventLogPageComp
                                default: return null
                                }
                            }
                        }
                    }

                    Component {
                        id: homePageComp
                        HomePage {
                            onHomeNavigate: function(index) {
                                pageContainer.switchPage(index)
                            }
                        }
                    }
                    Component {
                        id: backupPageComp
                        BackupPage {
                            onNavigateHomeRequested: pageContainer.switchPage(0)
                        }
                    }
                    Component {
                        id: restorePageComp
                        RestorePage {
                            onNavigateHomeRequested: pageContainer.switchPage(0)
                        }
                    }
                    Component { id: mountPageComp; MountPage { } }
                    Component { id: repositoryPageComp; RepositoryPage { } }
                    Component { id: eventLogPageComp; EventLogPage { } }
                }

                // Settings right drawer (does not replace main page)
                Item {
                    id: settingsDrawer
                    anchors.fill: parent
                    z: 2500

                    Rectangle {
                        anchors.fill: parent
                        color: Theme.colorScrim
                        opacity: window.settingsPanelOpen ? 1 : 0
                        visible: opacity > 0.01
                        Behavior on opacity { NumberAnimation { duration: 250 } }
                        MouseArea {
                            anchors.fill: parent
                            enabled: window.settingsPanelOpen
                            onClicked: window.settingsPanelOpen = false
                        }
                    }

                    Rectangle {
                        id: settingsPanel
                        width: Math.max(400, Math.min(parent.width * 0.55, 640))
                        height: parent.height
                        property real slideProgress: window.settingsPanelOpen ? 0 : 1
                        x: parent.width - width + slideProgress * width
                        visible: slideProgress < 0.999 || window.settingsPanelOpen
                        color: Theme.colorBg
                        border.width: 1
                        border.color: Theme.colorBorder
                        Behavior on slideProgress {
                            NumberAnimation {
                                duration: 280
                                easing.type: Easing.OutCubic
                            }
                        }

                        SettingsPage {
                            anchors.fill: parent
                            onCloseRequested: window.settingsPanelOpen = false
                        }
                    }
                }
            }
        }
    }

    SplashOverlay {
        id: splash
        anchors.fill: parent
        windowAppReady: window.appReady
        onSizeHintChanged: window.applySplashSize()
        onQuitRequested: window.close()
    }

    // Global loading overlay (old Main.qml): menu switch / catalog reload / busy commands.
    LoadingOverlay {
        anchors.fill: parent
        z: 500
        visible: window.appLoading
        //% "Loading"
        message: qsTrId("aegra.common.loading")
    }

    ToastBanner {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: window.appReady ? 32 : 0
        visible: window.appReady
    }

    ResizeHandle {
        targetWindow: window
        edges: Qt.TopEdge
        resizeCursor: Qt.SizeVerCursor
        visible: window.canResize
        height: window.resizeBorder
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
    }
    ResizeHandle {
        targetWindow: window
        edges: Qt.BottomEdge
        resizeCursor: Qt.SizeVerCursor
        visible: window.canResize
        height: window.resizeBorder
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
    ResizeHandle {
        targetWindow: window
        edges: Qt.LeftEdge
        resizeCursor: Qt.SizeHorCursor
        visible: window.canResize
        width: window.resizeBorder
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
    }
    ResizeHandle {
        targetWindow: window
        edges: Qt.RightEdge
        resizeCursor: Qt.SizeHorCursor
        visible: window.canResize
        width: window.resizeBorder
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
