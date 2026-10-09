import QtQuick

// Border for a rounded panel (pill navbar, floating chat header/composer)
// that actually traces the full rounded silhouette — full 1px strength
// along the straight top/bottom edges, tapering (both thinner and fainter)
// as it sweeps around each corner arc, down to a faint minimum at the
// side. A plain uniform Rectangle.border looks wrong on a pill: this
// keeps the stroke going all the way around instead of just cutting a
// straight line off before it reaches the curve.
Canvas {
    id: root

    anchors.fill: parent
    antialiasing: true

    property real cornerRadius: parent && parent.radius !== undefined ? parent.radius : 0
    property color edgeColor: Qt.rgba(1, 1, 1, 0.08)
    property real edgeThickness: 1
    // Floor the taper hits at the side of the curve, as a fraction of
    // edgeThickness / edgeColor's own alpha. Kept just above zero so the
    // stroke reads as a continuous loop rather than dead-ending.
    property real minThicknessFactor: 0.3
    property real minAlphaFactor: 0.12
    property int cornerSteps: 24

    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onCornerRadiusChanged: requestPaint()
    onEdgeColorChanged: requestPaint()
    onEdgeThicknessChanged: requestPaint()

    onPaint: {
        var ctx = getContext("2d")
        ctx.reset()
        if (width <= 0 || height <= 0)
            return

        var r = Math.max(0, Math.min(cornerRadius, width / 2, height / 2))
        var t = edgeThickness
        var inset = t / 2
        var w = width
        var h = height

        function withAlpha(c, factor) {
            return Qt.rgba(c.r, c.g, c.b, c.a * factor)
        }
        // Smoothstep easing so the taper accelerates/decelerates instead
        // of moving at a mechanical constant rate.
        function ease(p) { return p * p * (3 - 2 * p) }

        function strokeAtLevel(level) {
            var alphaFactor = minAlphaFactor + (1 - minAlphaFactor) * level
            var widthFactor = minThicknessFactor + (1 - minThicknessFactor) * level
            ctx.strokeStyle = withAlpha(edgeColor, alphaFactor)
            ctx.lineWidth = t * widthFactor
        }

        function strokeStraight(x0, y0, x1, y1, level) {
            if (Math.hypot(x1 - x0, y1 - y0) <= 0)
                return
            strokeAtLevel(level)
            ctx.beginPath()
            ctx.moveTo(x0, y0)
            ctx.lineTo(x1, y1)
            ctx.stroke()
        }

        // fadeIn: level goes 0 -> 1 across the arc (min -> full).
        // !fadeIn: level goes 1 -> 0 across the arc (full -> min).
        function strokeCorner(cx, cy, startAngle, endAngle, fadeIn) {
            if (r <= 0)
                return
            for (var i = 0; i < cornerSteps; i++) {
                var p0 = i / cornerSteps
                var p1 = (i + 1) / cornerSteps
                var mid = ease((p0 + p1) / 2)
                strokeAtLevel(fadeIn ? mid : (1 - mid))
                var a0 = startAngle + (endAngle - startAngle) * p0
                var a1 = startAngle + (endAngle - startAngle) * p1
                ctx.beginPath()
                ctx.arc(cx, cy, r - inset, a0, a1)
                ctx.stroke()
            }
        }

        // "butt" caps matter here: the corners are stroked as many short
        // abutting arc segments (each a separate stroke() call, since
        // lineWidth/alpha step between them). Round caps would center a
        // little circle at every segment joint, and where the width steps
        // down that circle pokes out past the thinner neighbor — a visible
        // string of dots along the curve. Butt caps end exactly on the
        // shared arc path with nothing to poke out.
        ctx.lineCap = "butt"

        // Top / bottom straight edges — full strength.
        strokeStraight(r, inset, w - r, inset, 1)
        strokeStraight(r, h - inset, w - r, h - inset, 1)
        // Left / right straight edges (only present if height > 2*radius,
        // e.g. the taller composer strips) — held at the faded floor the
        // corners taper down to, for a seamless loop.
        strokeStraight(inset, r, inset, h - r, 0)
        strokeStraight(w - inset, r, w - inset, h - r, 0)

        // Corners: fade DOWN leaving the top/bottom edge, fade UP arriving
        // back into the next one.
        strokeCorner(w - r, r, 1.5 * Math.PI, 2 * Math.PI, false)   // top-right
        strokeCorner(w - r, h - r, 0, 0.5 * Math.PI, true)          // bottom-right
        strokeCorner(r, h - r, 0.5 * Math.PI, Math.PI, false)       // bottom-left
        strokeCorner(r, r, Math.PI, 1.5 * Math.PI, true)            // top-left
    }
}
