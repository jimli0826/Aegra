import QtQuick 2.15
import ".."

// Self-drawn caption controls (not system Segoe glyphs).
// Roles: minimize | maximize | restore | close
Rectangle {
    id: root
    property string role: "minimize"
    property bool isClose: role === "close"
    signal clicked()

    width: 40
    height: 28
    radius: 8
    color: btnMouse.containsMouse
           ? (isClose ? Theme.colorHoverClose : Theme.colorHover)
           : "transparent"

    readonly property color iconColor: isClose && btnMouse.containsMouse
                                       ? "#ffffff"
                                       : Theme.colorTextWhite

    Canvas {
        id: icon
        anchors.centerIn: parent
        width: 12
        height: 12
        antialiasing: true
        renderTarget: Canvas.FramebufferObject
        renderStrategy: Canvas.Cooperative

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.clearRect(0, 0, width, height)
            ctx.strokeStyle = root.iconColor
            ctx.fillStyle = "transparent"
            ctx.lineWidth = 1.5
            ctx.lineCap = "round"
            ctx.lineJoin = "round"

            switch (root.role) {
            case "minimize":
                ctx.beginPath()
                ctx.moveTo(1.5, 6.5)
                ctx.lineTo(10.5, 6.5)
                ctx.stroke()
                break
            case "maximize":
                ctx.beginPath()
                ctx.rect(1.75, 1.75, 8.5, 8.5)
                ctx.stroke()
                break
            case "restore":
                // Back square (top-right)
                ctx.beginPath()
                ctx.rect(3.25, 1.25, 7, 7)
                ctx.stroke()
                // Front square (bottom-left), clear overlap then redraw
                ctx.clearRect(1.25, 3.25, 7, 7)
                ctx.beginPath()
                ctx.rect(1.25, 3.25, 7, 7)
                ctx.stroke()
                break
            case "close":
                ctx.beginPath()
                ctx.moveTo(2, 2)
                ctx.lineTo(10, 10)
                ctx.moveTo(10, 2)
                ctx.lineTo(2, 10)
                ctx.stroke()
                break
            }
        }
    }

    onRoleChanged: icon.requestPaint()
    onIconColorChanged: icon.requestPaint()

    MouseArea {
        id: btnMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
        onContainsMouseChanged: icon.requestPaint()
    }
}
