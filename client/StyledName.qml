import QtQuick
import QtQuick.Effects
import "NameStyleParser.js" as NSP

// -----------------------------------------------------------------------------
// StyledName
//
// Renders "First Last" with an optional unique name style (see NameStyleParser).
// Behaves like a Text for layout purposes (implicit size, elide, alignment) so
// it can be dropped in where a Text used to live.
//
// The glyph shapes are anchored by a native Text (hinted, crisp at any size —
// the same rendering as a plain name), which prevents the speckles/holes at
// glyph junctions that unhinted Canvas text produced on small sizes:
//   * plain / solid  -> the native Text IS the visible fill.
//   * gradient        -> the native Text is drawn in the gradient's average
//     color (so it fills the glyph junctions solidly), and a Canvas draws the
//     real gradient text on top of it.
//
// Shadows/glow are drawn in a separate Canvas that overflows the item so the
// glow has room without changing the item's layout size.
// -----------------------------------------------------------------------------
Item {
    id: root

    property string firstName: ""
    property string lastName: ""
    property string styleCss: ""

    property color fallbackColor: "white"
    property int pixelSize: 14
    property string fontFamily: "Roboto"
    property int fontWeight: Font.Medium

    property int elide: Text.ElideNone
    property int horizontalAlignment: Text.AlignLeft
    property int verticalAlignment: Text.AlignVCenter
    property int wrapMode: Text.NoWrap

    readonly property string fullName: {
        var f = firstName ? firstName : ""
        var l = lastName ? lastName : ""
        return (f + " " + l).trim()
    }
    readonly property var parsed: NSP.parse(styleCss)
    readonly property bool styled: parsed.valid && parsed.styled
    readonly property bool useGradient: styled && parsed.useGradient
    readonly property bool hasShadows: styled && parsed.shadows.length > 0

    // Color of the native Text: for a gradient it's the average of the stops so
    // it backs the (unhinted) Canvas gradient and keeps junctions filled.
    readonly property color fillColor: {
        if (!styled)
            return fallbackColor
        if (useGradient)
            return NSP.averageColor(parsed.gradientStops)
        return parsed.solidColor.length > 0 ? parsed.solidColor : fallbackColor
    }

    // CSS font-weight for the Canvas 2D context.
    readonly property string cssFontWeight: {
        if (fontWeight >= Font.Bold) return "700"
        if (fontWeight >= Font.DemiBold) return "600"
        if (fontWeight >= Font.Medium) return "500"
        if (fontWeight >= Font.Normal) return "400"
        return "300"
    }

    // How far the glow Canvas extends past the item on every side. Does NOT
    // change the item's own size, so layout matches a plain Text exactly.
    readonly property real glowPad: hasShadows ? Math.ceil(pixelSize * 0.6) : 0

    // Supersample factor for the gradient mask texture. On a low-DPI display the
    // mask has only ~1px of anti-aliasing to work with (slightly jagged edges);
    // rendering it larger + mipmapped gives the effect a smoother, averaged edge.
    // The factor scales inversely with font size (small text has few absolute
    // pixels, so it needs more supersampling), targeting a roughly constant mask
    // resolution. Never below the device pixel ratio (so high-DPI/mobile isn't
    // downgraded) and capped so textures stay reasonable.
    readonly property real maskSupersample: Math.min(6, Math.max(2, Screen.devicePixelRatio, 144 / pixelSize))

    implicitWidth: baseText.implicitWidth
    implicitHeight: baseText.implicitHeight

    onParsedChanged: { glowCanvas.requestPaint(); gradientCanvas.requestPaint() }
    onFullNameChanged: { glowCanvas.requestPaint(); gradientCanvas.requestPaint() }
    onWidthChanged: { glowCanvas.requestPaint(); gradientCanvas.requestPaint() }
    onHeightChanged: { glowCanvas.requestPaint(); gradientCanvas.requestPaint() }
    onFallbackColorChanged: glowCanvas.requestPaint()

    // Shared: elide `text` to `avail` px using the Canvas 2D `ctx` measure.
    function elidedFor(ctx, avail) {
        var text = root.fullName
        if (root.elide !== Text.ElideNone && ctx.measureText(text).width > avail) {
            var ell = "…"
            while (text.length > 1 && ctx.measureText(text + ell).width > avail)
                text = text.substring(0, text.length - 1)
            text = text + ell
        }
        return text
    }

    // Shared: x for `text` of width `tw` inside [pad, pad+avail] given alignment.
    function xFor(tw, avail, pad) {
        if (root.horizontalAlignment === Text.AlignHCenter)
            return pad + Math.max(0, (avail - tw) / 2)
        if (root.horizontalAlignment === Text.AlignRight)
            return pad + Math.max(0, avail - tw)
        return pad
    }

    // Metrics of the glyph font, so the Canvas passes land on the exact same
    // baseline as the native Text (which centers its line box with AlignVCenter).
    FontMetrics {
        id: fm
        font.pixelSize: root.pixelSize
        font.family: root.fontFamily
        font.weight: root.fontWeight
    }

    // Baseline Y (in a canvas whose top is `padTop` above the item's top) that
    // matches the native Text's single-line AlignVCenter baseline. Use with
    // ctx.textBaseline = "alphabetic".
    function baselineY(canvasHeight, padTop) {
        var itemH = canvasHeight - 2 * padTop
        return padTop + (itemH - fm.height) / 2 + fm.ascent
    }

    // ---- glow / shadows (behind everything, overflowing for room) ----------
    Canvas {
        id: glowCanvas
        anchors.fill: parent
        anchors.margins: -root.glowPad
        visible: root.hasShadows
        Component.onCompleted: requestPaint()

        function toCss(qmlColor) {
            if (qmlColor && qmlColor.length > 0)
                return NSP.cssColor(qmlColor)
            var c = root.fallbackColor
            return "rgba(" + Math.round(c.r * 255) + "," + Math.round(c.g * 255)
                    + "," + Math.round(c.b * 255) + "," + c.a + ")"
        }

        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            if (!root.hasShadows || width <= 0 || height <= 0)
                return
            var shadows = root.parsed.shadows
            var count = Math.min(shadows.length, 8)
            if (count === 0)
                return

            var pad = root.glowPad
            ctx.font = root.cssFontWeight + " " + root.pixelSize + "px \""
                    + root.fontFamily + "\", sans-serif"
            ctx.textBaseline = "alphabetic"

            var availW = width - 2 * pad
            if (availW < 1) availW = width
            var text = root.elidedFor(ctx, availW)
            var tw = ctx.measureText(text).width
            var x = root.xFor(tw, availW, pad)
            var y = root.baselineY(height, pad)

            // "Shadow only": draw the casting body far off-screen and use
            // shadowOffsetX to bring just its (blurred) shadow into place.
            var off = width + 48
            for (var s = 0; s < count; ++s) {
                var sh = shadows[s]
                ctx.save()
                ctx.shadowColor = glowCanvas.toCss(sh.color)
                ctx.shadowBlur = sh.blur
                ctx.shadowOffsetX = sh.dx + off
                ctx.shadowOffsetY = sh.dy
                ctx.fillStyle = "#000000"
                ctx.fillText(text, x - off, y)
                ctx.restore()
            }
        }
    }

    // ---- the glyphs: native Text (crisp/hinted) ----------------------------
    // Plain & solid: the visible fill. Gradient: the average-colour backing AND
    // the mask that shapes the gradient (its hinted alpha is what keeps glyph
    // junctions fully covered, so no backing colour speckles through).
    Text {
        id: baseText
        anchors.fill: parent
        text: root.fullName
        color: root.fillColor
        font.pixelSize: root.pixelSize
        font.family: root.fontFamily
        font.weight: root.fontWeight
        horizontalAlignment: root.horizontalAlignment
        verticalAlignment: root.verticalAlignment
        elide: root.elide
        wrapMode: root.wrapMode
        // Rendered into a layer so it can be used as the gradient's mask, at a
        // supersampled + mipmapped size so its anti-aliased edges stay smooth
        // even on low-DPI displays.
        layer.enabled: root.useGradient
        layer.smooth: true
        layer.mipmap: true
        layer.textureSize: Qt.size(Math.max(1, baseText.width) * root.maskSupersample,
                                   Math.max(1, baseText.height) * root.maskSupersample)
        onContentWidthChanged: gradientCanvas.requestPaint()
        onContentHeightChanged: gradientCanvas.requestPaint()
    }

    // ---- gradient fill: a smooth gradient rectangle masked to the hinted
    // native glyphs -----------------------------------------------------------
    // The Canvas only draws a rectangle (no text), so it has nothing to speckle;
    // the glyph shape — including junctions — comes from the hinted native Text
    // via the mask. This is the same masking the app uses for circular avatars.
    Canvas {
        id: gradientCanvas
        anchors.fill: parent
        visible: root.useGradient
        layer.enabled: root.useGradient
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: baseText
            // Preserve the glyphs' anti-aliased edges instead of hard-cutting
            // them (the default threshold 0 / spread 0 makes every faint edge
            // pixel fully opaque → bold, jagged text). A mid threshold with a
            // full spread ramps the mask smoothly across the AA range.
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
            antialiasing: true
        }
        Component.onCompleted: requestPaint()

        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            if (!root.useGradient || width <= 0 || height <= 0)
                return
            var stops = root.parsed.gradientStops
            if (!stops || stops.length < 2)
                return

            // Position the gradient over the actual glyph run (the native Text's
            // content width + alignment) so the angle/stops land on the letters.
            var cw = baseText.contentWidth
            if (cw <= 0) cw = width
            var tx = root.xFor(cw, width, 0)

            var a = root.parsed.gradientAngle * Math.PI / 180.0
            var dirX = Math.sin(a)
            var dirY = -Math.cos(a)
            var textH = root.pixelSize * 1.3
            var lineLen = Math.abs(cw * Math.sin(a)) + Math.abs(textH * Math.cos(a))
            var cx = tx + cw / 2.0
            var cy = height / 2.0
            var grad = ctx.createLinearGradient(cx - dirX * lineLen / 2.0,
                                                cy - dirY * lineLen / 2.0,
                                                cx + dirX * lineLen / 2.0,
                                                cy + dirY * lineLen / 2.0)
            // Two stops at the exact same position (a hard color stop, e.g.
            // "red 32%, orange 32%") get merged by Qt's gradient into just the
            // last one. Nudging equal positions apart by a tiny epsilon keeps
            // the transition near-instant — i.e. renders it as a hard edge.
            var prevP = -1
            for (var i = 0; i < stops.length; ++i) {
                var p = stops[i].pos
                if (p < 0) p = 0
                if (p > 1) p = 1
                if (p <= prevP) p = Math.min(1, prevP + 0.0001)
                prevP = p
                grad.addColorStop(p, NSP.cssColor(stops[i].color))
            }
            ctx.fillStyle = grad
            ctx.fillRect(0, 0, width, height)
        }
    }
}
