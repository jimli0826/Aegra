import QtQuick 2.15
import QtQuick.Controls 2.15
import ".."

Rectangle {
    id: root
    visible: serviceClient.globalLoading && !serviceClient.splashVisible
    anchors.fill: parent
    color: "#66000000"
    z: 800
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
