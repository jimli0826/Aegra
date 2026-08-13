import QtQuick 2.15

// Lucide-style outline icons for the glass sidebar.
// Qt Canvas ellipse is ellipse(x, y, w, h) — NOT the HTML5 center/radius form.
Item {
    id: root
    property string name: "home"
    property color color: "#5a7572"
    property real strokeWidth: 1.85
    // Match concept mock: 18×18 glyph in a fixed nav slot.
    width: 18
    height: 18

    onNameChanged: canvas.requestPaint()
    onColorChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    onStrokeWidthChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true
        renderTarget: Canvas.FramebufferObject
        renderStrategy: Canvas.Cooperative

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.clearRect(0, 0, width, height)
            // Design grid is 24×24; scale into item size.
            var s = width / 24
            ctx.scale(s, s)
            ctx.strokeStyle = root.color
            ctx.fillStyle = root.color
            ctx.lineWidth = root.strokeWidth
            ctx.lineCap = "round"
            ctx.lineJoin = "round"

            function rrect(x, y, w, h, r) {
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
                ctx.stroke()
            }

            // Qt: ellipse(x, y, w, h) — bounding box, not center radii.
            function oval(x, y, w, h) {
                ctx.beginPath()
                ctx.ellipse(x, y, w, h)
                ctx.stroke()
            }

            function circle(cx, cy, r) {
                ctx.beginPath()
                ctx.arc(cx, cy, r, 0, Math.PI * 2)
                ctx.stroke()
            }

            switch (root.name) {
            case "home": // layout-dashboard
                rrect(3, 3, 8, 8, 1.5)
                rrect(13, 3, 8, 5, 1.5)
                rrect(13, 12, 8, 9, 1.5)
                rrect(3, 15, 8, 6, 1.5)
                break

            case "backup": // database-backup (cylinder + refresh arc)
                // Cylinder body
                oval(4, 4, 12, 5)
                ctx.beginPath()
                ctx.moveTo(4, 6.5)
                ctx.lineTo(4, 16)
                ctx.ellipse(4, 14, 12, 5)
                ctx.moveTo(16, 6.5)
                ctx.lineTo(16, 16.5)
                ctx.stroke()
                // Mid ring
                ctx.beginPath()
                ctx.ellipse(4, 9, 12, 5)
                ctx.stroke()
                // Small backup arc (right)
                ctx.beginPath()
                ctx.arc(18.5, 14, 3.2, -Math.PI * 0.85, Math.PI * 0.55, false)
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(20.5, 11.2)
                ctx.lineTo(21.5, 13.2)
                ctx.lineTo(19.2, 13.6)
                ctx.stroke()
                break

            case "restore": // history
                circle(12, 13, 7.5)
                ctx.beginPath()
                ctx.moveTo(12, 9)
                ctx.lineTo(12, 13)
                ctx.lineTo(15, 15)
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(5.5, 7.5)
                ctx.lineTo(5.5, 4.5)
                ctx.lineTo(8.5, 4.5)
                ctx.stroke()
                break

            case "mount": // hard-drive
                rrect(3, 9, 18, 9, 2)
                ctx.beginPath()
                ctx.moveTo(3, 13.5)
                ctx.lineTo(21, 13.5)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(7.5, 16.5, 0.9, 0, Math.PI * 2)
                ctx.fill()
                ctx.beginPath()
                ctx.arc(11, 16.5, 0.9, 0, Math.PI * 2)
                ctx.fill()
                break

            case "folder": // lucide folder
                ctx.beginPath()
                ctx.moveTo(3.5, 8)
                ctx.lineTo(3.5, 18.5)
                ctx.quadraticCurveTo(3.5, 20, 5, 20)
                ctx.lineTo(19, 20)
                ctx.quadraticCurveTo(20.5, 20, 20.5, 18.5)
                ctx.lineTo(20.5, 9.5)
                ctx.quadraticCurveTo(20.5, 8, 19, 8)
                ctx.lineTo(11.5, 8)
                ctx.lineTo(9.5, 5.5)
                ctx.quadraticCurveTo(9, 5, 8.2, 5)
                ctx.lineTo(5, 5)
                ctx.quadraticCurveTo(3.5, 5, 3.5, 6.5)
                ctx.closePath()
                ctx.stroke()
                break

            case "hard_drive": // lucide hard-drive (volume_set type mark)
                rrect(2.5, 8, 19, 10, 2)
                ctx.beginPath()
                ctx.moveTo(2.5, 13)
                ctx.lineTo(21.5, 13)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(7, 16.2, 1.1, 0, Math.PI * 2)
                ctx.fill()
                ctx.beginPath()
                ctx.arc(11, 16.2, 1.1, 0, Math.PI * 2)
                ctx.fill()
                break

            case "repository": // archive
                rrect(3, 3.5, 18, 4.5, 1.2)
                ctx.beginPath()
                ctx.moveTo(5, 8)
                ctx.lineTo(5, 19.5)
                ctx.lineTo(19, 19.5)
                ctx.lineTo(19, 8)
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(10, 13)
                ctx.lineTo(14, 13)
                ctx.stroke()
                break

            case "event_log": // scroll-text
                // Scroll body
                ctx.beginPath()
                ctx.moveTo(7, 5)
                ctx.quadraticCurveTo(7, 3, 9, 3)
                ctx.lineTo(16, 3)
                ctx.quadraticCurveTo(18, 3, 18, 5)
                ctx.lineTo(18, 17)
                ctx.quadraticCurveTo(18, 19, 16, 19)
                ctx.lineTo(9, 19)
                ctx.quadraticCurveTo(6, 19, 6, 16.5)
                ctx.lineTo(6, 7)
                ctx.stroke()
                // Roll curl
                ctx.beginPath()
                ctx.moveTo(6, 7)
                ctx.quadraticCurveTo(9, 7, 9, 5)
                ctx.stroke()
                // Text lines
                ctx.beginPath()
                ctx.moveTo(10, 9)
                ctx.lineTo(15.5, 9)
                ctx.moveTo(10, 12)
                ctx.lineTo(15.5, 12)
                ctx.moveTo(10, 15)
                ctx.lineTo(14, 15)
                ctx.stroke()
                break

            case "settings": // settings-2 (sliders)
                ctx.beginPath()
                ctx.moveTo(4, 7)
                ctx.lineTo(20, 7)
                ctx.moveTo(4, 12)
                ctx.lineTo(20, 12)
                ctx.moveTo(4, 17)
                ctx.lineTo(20, 17)
                ctx.stroke()
                // Knobs as small rounded rects (clearer than filled dots)
                rrect(7.5, 5, 4, 4, 1.2)
                rrect(13.5, 10, 4, 4, 1.2)
                rrect(9.5, 15, 4, 4, 1.2)
                break

            case "feedback": // message-circle
                circle(12, 11, 7.5)
                ctx.beginPath()
                ctx.moveTo(8.5, 16.5)
                ctx.quadraticCurveTo(7.5, 19, 6, 20.5)
                ctx.quadraticCurveTo(9, 19.5, 11.5, 17.5)
                ctx.stroke()
                break

            case "refresh": // refresh-cw (Lucide standard)
                // Top arc & arrow
                ctx.beginPath()
                ctx.arc(12, 12, 9, Math.PI, Math.PI * 1.5, false)
                ctx.lineTo(21, 8)
                ctx.stroke()

                ctx.beginPath()
                ctx.moveTo(21, 3)
                ctx.lineTo(21, 8)
                ctx.lineTo(16, 8)
                ctx.stroke()

                // Bottom arc & arrow
                ctx.beginPath()
                ctx.arc(12, 12, 9, 0, Math.PI * 0.5, false)
                ctx.lineTo(3, 16)
                ctx.stroke()

                ctx.beginPath()
                ctx.moveTo(3, 21)
                ctx.lineTo(3, 16)
                ctx.lineTo(8, 16)
                ctx.stroke()
                break

            case "bell": // bell (Lucide outline)
                ctx.beginPath()
                ctx.moveTo(18, 8)
                ctx.arc(12, 8, 6, 0, Math.PI, true)
                ctx.quadraticCurveTo(6, 15, 3, 17)
                ctx.lineTo(21, 17)
                ctx.quadraticCurveTo(18, 15, 18, 8)
                ctx.stroke()

                ctx.beginPath()
                ctx.arc(12, 17, 2.5, Math.PI * 0.15, Math.PI * 0.85, false)
                ctx.stroke()
                break

            default:
                rrect(5, 5, 14, 14, 2)
                break
            }
        }
    }
}
