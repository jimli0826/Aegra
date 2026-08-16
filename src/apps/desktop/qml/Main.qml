import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import "."
import "components"
import "pages"

// Shell chrome aligned with backup/src/gui Main.qml (title bar, sidebar, page fade).
// Starts as compact splash window (600×460), then expands to main UI (1080×720).
Window {
    id: window
    // Start as splash window only (compact card; not full main UI frame)
    width: 600
    height: 460
    visible: true
    // Keep the native caption empty. Branding is drawn by SidebarMenu and the
    // application name still identifies the process in the taskbar.
    title: ""
    // Acrylic and the QML rounded fallback need a transparent clear color. Without
    // Acrylic, Windows 11 clears opaquely and lets DWM clip the native corners.
    color: surfaceColor
    flags: Qt.Window | Qt.FramelessWindowHint
    minimumWidth: appReady ? 900 : 400
    minimumHeight: appReady ? 600 : 300

    readonly property int mainWidth: 1080
    readonly property int mainHeight: 720
    readonly property int resizeBorder: 6
    readonly property bool hasNativeWindowCorners: nativeWindowCorners
    readonly property color surfaceColor: Theme.hasAcrylicBlur
                                          ? "transparent"
                                          : (hasNativeWindowCorners
                                             ? Theme.colorBg : "transparent")
    readonly property bool canResize: appReady
                                      && visibility !== Window.Maximized
                                      && visibility !== Window.FullScreen
    readonly property real chromeRadius: (visibility === Window.Maximized
                                          || visibility === Window.FullScreen
                                          || hasNativeWindowCorners)
                                         ? 0 : Theme.radiusWindow
    /// Settings opens as right drawer (old Main.qml pattern)
    property bool settingsPanelOpen: false
    /// Latched once splash dismisses so main opacity/animation never re-toggles.
    property bool appReady: false
    // App loading overlay is for heavy blocking commands only.
    // Exclude: sequential Repository Refresh probes (row-level Loading),
    // background catalog refreshes, and schedule enable/disable row toggles.
    readonly property bool appLoading: {
        if (!window.appReady || !serviceClient.connected)
            return false
        return (serviceClient.repositoryCommandBusy
                    && !serviceClient.repositoryRefreshRunning)
                || serviceClient.backupCommandBusy
                || serviceClient.cancelCommandBusy
                || serviceClient.mountCommandBusy
                || serviceClient.scheduleCommandBlocksUi
    }

    // Old Main.qml LoadingOverlay: always the same "Loading" string (never per-page text).
    readonly property string appLoadingMessage: qsTrId("aegra.common.loading")

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
        var h = splash.preferredHeight || 460
        if (window.width !== w || window.height !== h) {
            window.width = w
            window.height = h
            centerOnScreen()
        }
    }

    function applyMainSize() {
        window.width = window.mainWidth
        window.height = window.mainHeight
        centerOnScreen()
    }

    function enterMainUi() {
        if (window.appReady)
            return
        window.appReady = true
        splash.windowAppReady = true
        applyMainSize()
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

    // Use one rounded-corner renderer only: DWM on Windows 11, QML elsewhere.
    // A fully opaque square shell avoids stale alpha pixels after restore.
    Rectangle {
        id: shell
        anchors.fill: parent
        radius: window.chromeRadius
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: Theme.colorBg }
            GradientStop { position: 1.0; color: Theme.colorBgEnd }
        }
        border.width: 0
        clip: true

        // Full-height body (no system/title brand strip — brand lives in sidebar only).
        RowLayout {
            anchors.fill: parent
            anchors.margins: window.chromeRadius > 0 ? 1 : 0
            spacing: 0
            // Hidden under splash; fade in once when ready (never re-hide → no flicker).
            opacity: window.appReady ? 1 : 0
            enabled: window.appReady
            Behavior on opacity {
                enabled: window.appReady
                NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
            }

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
                // Transparent background so outer shell rounded corners show cleanly on the right side.
                color: "transparent"

                Item {
                    id: pageContainer
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.top: parent.top
                    anchors.topMargin: 50
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
                            // Restore reattaches summary progress from the live job list.
                            if (newIndex === 2)
                                serviceClient.refreshJobs()
                        } else if (newIndex === 4) {
                            serviceClient.refreshConnections()
                        } else if (newIndex === 5) {
                            // Task Log: terminal jobs via ListJobs scope=terminal.
                            if (serviceClient.jobListAvailable)
                                serviceClient.refreshTaskLog(0, 0, 0)
                        }
                    }

                    // 0 Home, 1 Backup, 2 Restore, 3 Mount, 4 Repository, 5 Task Log
                    Repeater {
                        model: 6
                        Loader {
                            anchors.fill: parent
                            opacity: index === pageContainer.currentIndex ? 1 : 0
                            visible: opacity > 0
                            // Keep Restore (index 2) alive so an in-flight restore session and
                            // multi-mapping start queue are not torn down when navigating away.
                            active: index === pageContainer.currentIndex
                                    || index === pageContainer.previousIndex
                                    || index === 2

                            Behavior on opacity {
                                NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
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
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.topMargin: 60
                        anchors.bottomMargin: 0
                        property real slideProgress: window.settingsPanelOpen ? 0 : 1
                        x: parent.width - width + slideProgress * width
                        visible: slideProgress < 0.999 || window.settingsPanelOpen
                        color: Theme.colorCard
                        radius: Theme.radiusWindow
                        clip: true
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

        // Floating caption: drag region + self-drawn window buttons only (no logo / version).
        Item {
            id: captionBar
            z: 200
            height: 40
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            visible: window.appReady

            MouseArea {
                anchors.fill: parent
                anchors.topMargin: window.canResize ? window.resizeBorder : 0
                anchors.rightMargin: 200
                onPressed: window.startSystemMove()
                onDoubleClicked: {
                    if (window.visibility === Window.Maximized)
                        window.showNormal()
                    else
                        window.showMaximized()
                }
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                // Global Notification Bell Button (immediately to the left of minimize button)
                Rectangle {
                    id: globalNotifBtn
                    width: 32
                    height: 28
                    radius: 8
                    color: globalNotifMouse.pressed ? Theme.colorButtonHover : (globalNotifMouse.containsMouse ? Theme.colorHover : "transparent")
                    border.width: 0

                    NavIcon {
                        anchors.centerIn: parent
                        width: 16
                        height: 16
                        name: "bell"
                        color: globalNotifMouse.containsMouse ? Theme.colorAccentBlue : Theme.colorTextGrey
                    }

                    Rectangle {
                        width: 7
                        height: 7
                        radius: 3.5
                        color: "#EE6476"
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.topMargin: 3
                        anchors.rightMargin: 3
                    }

                    MouseArea {
                        id: globalNotifMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            pageContainer.switchPage(5)
                        }
                    }
                }

                WindowButton {
                    role: "minimize"
                    onClicked: window.showMinimized()
                }
                WindowButton {
                    role: window.visibility === Window.Maximized ? "restore" : "maximize"
                    onClicked: window.visibility === Window.Maximized
                               ? window.showNormal() : window.showMaximized()
                }
                WindowButton {
                    role: "close"
                    onClicked: window.close()
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

        // Global loading overlay (old Main.qml): bounded control-plane queries / busy commands.
        LoadingOverlay {
            anchors.fill: parent
            z: 8000
            visible: window.appLoading
            //% "Loading"
            message: window.appLoadingMessage
        }

        ToastBanner {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: window.appReady ? 44 : 0
            visible: window.appReady
        }
    } // shell

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
