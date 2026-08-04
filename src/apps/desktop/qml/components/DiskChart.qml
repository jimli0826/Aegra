import QtQuick 2.15
import ".."

Column {
    id: root
    property real percent: 0.5
    property string label: ""
    property string diskName: ""
    
    // 动画相关属性
    property real animatedPercent: 0
    property bool animationEnabled: true
    
    // 用于数值动画的属性（从label中提取数值）
    property real targetValue: parseFloat(label) || 0
    property real animatedValue: 0
    property string unit: label.replace(/[0-9.]/g, '').trim()

    spacing: 6
    
    function playUsageAnimation() {
        if (!animationEnabled) {
            animatedPercent = percent
            animatedValue = targetValue
            return
        }
        // Restart from zero so the used-space arc is visible (old Home).
        animatedPercent = 0
        animatedValue = 0
        animationTimer.restart()
    }

    // 组件加载完成后启动动画
    Component.onCompleted: playUsageAnimation()

    // When bound data arrives asynchronously (API), re-apply targets
    onPercentChanged: playUsageAnimation()
    onTargetValueChanged: playUsageAnimation()
    onVisibleChanged: {
        if (visible)
            playUsageAnimation()
    }

    // 延迟启动动画，让UI先渲染
    Timer {
        id: animationTimer
        interval: 120
        onTriggered: {
            animatedPercent = percent
            animatedValue = targetValue
        }
    }
    
    // percent 动画
    Behavior on animatedPercent {
        NumberAnimation {
            duration: 800
            easing.type: Easing.OutCubic
        }
    }
    
    // 数值动画
    Behavior on animatedValue {
        NumberAnimation {
            duration: 800
            easing.type: Easing.OutCubic
        }
    }

    Item {
        width: 80
        height: 80

        Canvas {
            id: diskCanvas
            anchors.fill: parent
            
            // 当动画值变化时重绘
            property real displayPercent: root.animatedPercent
            onDisplayPercentChanged: requestPaint()

            // Theme-aware track / progress colors
            property color trackColor: Theme.colorProgressTrack
            property color progressColor: Theme.colorAccentBlue
            onTrackColorChanged: requestPaint()
            onProgressColorChanged: requestPaint()

            onPaint: {
                var ctx = getContext("2d")
                var cx = width / 2
                var cy = height / 2
                var r = 32
                var lw = 6

                ctx.reset()
                ctx.lineCap = "round"

                // Background circle (theme progress track)
                ctx.beginPath()
                ctx.arc(cx, cy, r, 0, 2 * Math.PI)
                ctx.strokeStyle = trackColor
                ctx.lineWidth = lw
                ctx.stroke()

                // Progress arc - 使用动画值
                ctx.beginPath()
                ctx.arc(cx, cy, r, -Math.PI / 2, -Math.PI / 2 + displayPercent * 2 * Math.PI)
                ctx.strokeStyle = progressColor
                ctx.lineWidth = lw
                ctx.stroke()
            }

            Component.onCompleted: requestPaint()
        }

        // 文字使用动画数值显示
        Text {
            anchors.centerIn: parent
            text: root.animatedValue.toFixed(1) + " " + root.unit
            color: Theme.colorTextWhite
            font.pixelSize: 11
            font.bold: true
            font.family: Theme.fontFamily
        }
    }

    Text {
        text: root.diskName
        color: Theme.colorTextGrey
        font.pixelSize: 12
        font.family: Theme.fontFamily
        anchors.horizontalCenter: parent.horizontalCenter
    }
}
