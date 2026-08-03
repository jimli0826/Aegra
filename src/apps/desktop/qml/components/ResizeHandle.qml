import QtQuick 2.15

Item {
    id: root
    required property var targetWindow
    property int edges: Qt.TopEdge
    property int resizeCursor: Qt.ArrowCursor

    z: 10000

    MouseArea {
        anchors.fill: parent
        cursorShape: root.resizeCursor
        onPressed: root.targetWindow.startSystemResize(root.edges)
    }
}
