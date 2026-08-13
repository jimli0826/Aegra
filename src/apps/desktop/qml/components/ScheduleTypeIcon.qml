import QtQuick 2.15

// Schedule SOURCE type mark — matches product outline folder / solid hard-drive glyphs.
Item {
    id: root
    property int size: 28
    /// "volume" | "files"
    property string kind: "volume"
    property color ink: "#6B7280"

    width: size
    height: size
    implicitWidth: size
    implicitHeight: size

    readonly property bool isFiles: kind === "files"

    Canvas {
        id: glyph
        anchors.centerIn: parent
        width: Math.round(size * 0.78)
        height: width
        antialiasing: true
        renderTarget: Canvas.FramebufferObject
        renderStrategy: Canvas.Cooperative

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.clearRect(0, 0, width, height)
            var s = width / 24
            ctx.scale(s, s)
            ctx.fillStyle = root.ink
            ctx.strokeStyle = root.ink
            ctx.lineCap = "round"
            ctx.lineJoin = "round"

            if (root.isFiles) {
                // Outline folder (Image #1 style)
                ctx.lineWidth = 1.9
                ctx.beginPath()
                // Tab
                ctx.moveTo(2.5, 8.2)
                ctx.lineTo(2.5, 6.4)
                ctx.quadraticCurveTo(2.5, 5.2, 3.7, 5.2)
                ctx.lineTo(9.0, 5.2)
                ctx.lineTo(11.0, 7.6)
                ctx.lineTo(20.3, 7.6)
                ctx.quadraticCurveTo(21.5, 7.6, 21.5, 8.8)
                // Body outline
                ctx.lineTo(21.5, 18.6)
                ctx.quadraticCurveTo(21.5, 19.8, 20.3, 19.8)
                ctx.lineTo(3.7, 19.8)
                ctx.quadraticCurveTo(2.5, 19.8, 2.5, 18.6)
                ctx.lineTo(2.5, 8.2)
                ctx.stroke()
                // Front lip line
                ctx.beginPath()
                ctx.moveTo(5.2, 12.6)
                ctx.lineTo(18.8, 12.6)
                ctx.stroke()
            } else {
                // Solid hard-drive (Image #2 style)
                ctx.lineWidth = 0
                // Drive body
                roundedRect(ctx, 2.2, 7.2, 19.6, 11.2, 3.2)
                ctx.fill()
                // Top plate highlight edge
                ctx.globalAlpha = 0.18
                ctx.fillStyle = "#ffffff"
                roundedRect(ctx, 3.4, 8.2, 17.2, 3.6, 1.6)
                ctx.fill()
                ctx.globalAlpha = 1.0
                // Activity LEDs
                ctx.fillStyle = "#ffffff"
                ctx.beginPath()
                ctx.arc(16.6, 15.0, 1.15, 0, Math.PI * 2)
                ctx.fill()
                ctx.beginPath()
                ctx.arc(19.2, 15.0, 1.15, 0, Math.PI * 2)
                ctx.fill()
            }
        }

        function roundedRect(ctx, x, y, w, h, r) {
            var rr = Math.min(r, w / 2, h / 2)
            ctx.beginPath()
            ctx.moveTo(x + rr, y)
            ctx.lineTo(x + w - rr, y)
            ctx.quadraticCurveTo(x + w, y, x + w, y + rr)
            ctx.lineTo(x + w, y + h - rr)
            ctx.quadraticCurveTo(x + w, y + h, x + w - rr, y + h)
            ctx.lineTo(x + rr, y + h)
            ctx.quadraticCurveTo(x, y + h, x, y + h - rr)
            ctx.lineTo(x, y + rr)
            ctx.quadraticCurveTo(x, y, x + rr, y)
            ctx.closePath()
        }

        Connections {
            target: root
            function onKindChanged() { glyph.requestPaint() }
            function onInkChanged() { glyph.requestPaint() }
            function onSizeChanged() { glyph.requestPaint() }
        }
        Component.onCompleted: requestPaint()
    }
}
