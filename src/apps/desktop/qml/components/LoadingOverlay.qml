import QtQuick 2.15
import QtQuick.Controls 2.15
import ".."

Rectangle {
    id: root
    // Full-screen dim only for catalog loads after main UI is up — not connect churn.
    // Parent may further gate with appReady.
    visible: serviceClient.globalLoading && !serviceClient.splashVisible
    anchors.fill: parent
    color: "#66000000"
    z: 800
    opacity: visible ? 1 : 0
    // Avoid hard show/hide flash when repository/inventory queries complete.
    Behavior on opacity {
        NumberAnimation { duration: 120 }
    }
    //% "Loading"
    Accessible.name: qsTrId("aegra.common.loading")
    Accessible.role: Accessible.Pane

    BusyIndicator {
        anchors.centerIn: parent
        running: root.visible
        width: 36
        height: 36
    }
}
