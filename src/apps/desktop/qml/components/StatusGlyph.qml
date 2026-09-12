import QtQuick 2.15
import QtQuick.Controls 2.15
import ".."

// Compact status mark aligned with Event Log: check / x / minus / spinner / clock / warning.
Item {
    id: root
    property int size: 16
    /// succeeded | failed | cancelled | running | queued | na | incomplete
    property string kind: "succeeded"
    property string label: ""

    width: size
    height: size
    implicitWidth: size
    implicitHeight: size
    Accessible.name: label

    HoverHandler {
        id: hover
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    }
    ToolTip.visible: hover.hovered && root.label.length > 0
    ToolTip.delay: 400
    ToolTip.text: root.label

    Item {
        id: spinner
        anchors.fill: parent
        property real spinAngle: 0
        rotation: root.kind === "running" ? spinAngle : 0

        NumberAnimation on spinAngle {
            running: root.kind === "running"
            from: 0
            to: 360
            loops: Animation.Infinite
            duration: 1200
        }

        Canvas {
            id: glyph
            anchors.fill: parent
            antialiasing: true
            renderTarget: Canvas.FramebufferObject
            renderStrategy: Canvas.Cooperative

            property string kind: root.kind
            property color greenColor: Theme.colorGreen
            property color redColor: Theme.colorAccentRed
            property color blueColor: Theme.colorAccentBlue
            property color greyColor: Theme.colorTextGrey

            onKindChanged: requestPaint()
            onGreenColorChanged: requestPaint()
            onRedColorChanged: requestPaint()
            onBlueColorChanged: requestPaint()
            onGreyColorChanged: requestPaint()
            Component.onCompleted: requestPaint()

            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                ctx.clearRect(0, 0, width, height)
                var cx = width / 2
                var cy = height / 2
                ctx.lineWidth = 1.6
                ctx.lineCap = "round"
                ctx.lineJoin = "round"

                if (kind === "succeeded") {
                    ctx.strokeStyle = greenColor
                    ctx.beginPath()
                    ctx.arc(cx, cy, 6.2, 0, Math.PI * 2)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.moveTo(cx - 3.4, cy - 0.1)
                    ctx.lineTo(cx - 0.8, cy + 2.4)
                    ctx.lineTo(cx + 3.6, cy - 2.4)
                    ctx.stroke()
                    return
                }
                if (kind === "failed") {
                    ctx.strokeStyle = redColor
                    ctx.beginPath()
                    ctx.arc(cx, cy, 6.2, 0, Math.PI * 2)
                    ctx.stroke()
                    var r = 2.6
                    ctx.beginPath()
                    ctx.moveTo(cx - r, cy - r)
                    ctx.lineTo(cx + r, cy + r)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.moveTo(cx + r, cy - r)
                    ctx.lineTo(cx - r, cy + r)
                    ctx.stroke()
                    return
                }
                if (kind === "cancelled") {
                    ctx.strokeStyle = "#e6a817"
                    ctx.beginPath()
                    ctx.arc(cx, cy, 6.2, 0, Math.PI * 2)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.moveTo(cx - 2.8, cy)
                    ctx.lineTo(cx + 2.8, cy)
                    ctx.stroke()
                    return
                }
                if (kind === "running") {
                    ctx.strokeStyle = blueColor
                    ctx.fillStyle = blueColor
                    var rr = width / 2 - 2.0
                    function drawArcArrow(startAng, endAng) {
                        ctx.beginPath()
                        ctx.arc(cx, cy, rr, startAng, endAng, false)
                        ctx.stroke()
                        var tipX = cx + Math.cos(endAng) * rr
                        var tipY = cy + Math.sin(endAng) * rr
                        var tang = endAng + Math.PI / 2
                        var hx = Math.cos(tang)
                        var hy = Math.sin(tang)
                        var nx = Math.cos(endAng)
                        var ny = Math.sin(endAng)
                        ctx.beginPath()
                        ctx.moveTo(tipX + hx * 3.2, tipY + hy * 3.2)
                        ctx.lineTo(tipX - nx * 2.1 - hx * 0.3, tipY - ny * 2.1 - hy * 0.3)
                        ctx.lineTo(tipX + nx * 2.1 - hx * 0.3, tipY + ny * 2.1 - hy * 0.3)
                        ctx.closePath()
                        ctx.fill()
                    }
                    drawArcArrow(-Math.PI * 0.85, -Math.PI * 0.15)
                    drawArcArrow(Math.PI * 0.15, Math.PI * 0.85)
                    return
                }
                if (kind === "queued") {
                    ctx.strokeStyle = greyColor
                    ctx.beginPath()
                    ctx.arc(cx, cy, 6.2, 0, Math.PI * 2)
                    ctx.stroke()
                    ctx.beginPath()
                    ctx.moveTo(cx, cy - 2.4)
                    ctx.lineTo(cx, cy)
                    ctx.lineTo(cx + 2.4, cy + 1.4)
                    ctx.stroke()
                    return
                }
                if (kind === "na") {
                    // Never run: a quiet dash, no ring, so it reads as "no data".
                    ctx.strokeStyle = greyColor
                    ctx.globalAlpha = 0.55
                    ctx.beginPath()
                    ctx.moveTo(cx - 3.6, cy)
                    ctx.lineTo(cx + 3.6, cy)
                    ctx.stroke()
                    ctx.globalAlpha = 1.0
                    return
                }
                ctx.strokeStyle = redColor
                ctx.fillStyle = redColor
                ctx.beginPath()
                ctx.moveTo(cx, cy - 6.4)
                ctx.lineTo(cx + 6.6, cy + 5.6)
                ctx.lineTo(cx - 6.6, cy + 5.6)
                ctx.closePath()
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(cx, cy - 2.2)
                ctx.lineTo(cx, cy + 1.4)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(cx, cy + 3.6, 0.9, 0, Math.PI * 2)
                ctx.fill()
            }
        }
    }
}
