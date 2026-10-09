.pragma library

// -----------------------------------------------------------------------------
// NameStyleParser
//
// Strict, whitelist-only parser for the small CSS subset used by "unique name
// styles". It converts a CSS-like string into a plain, fully-validated data
// object made only of numbers, enums and normalized color strings.
//
// SECURITY: the input string is NEVER rendered as markup. It is tokenized here
// and only known properties / value shapes are accepted; anything else makes the
// whole style invalid. Because the result is consumed as QML drawing properties
// (colors, offsets, gradient stops) and the visible text is always the plain
// "First Last", there is no HTML/rich-text surface and therefore no XSS vector.
//
// Accepted properties (everything else -> invalid):
//   color
//   background / background-image      (a single color OR linear-gradient(...))
//   background-clip / -webkit-background-clip   (only "text" is meaningful)
//   -webkit-text-fill-color / text-fill-color   (a color or "transparent")
//   text-shadow                        (comma separated "<x> <y> <blur> <color>")
//
// Accepted colors:
//   #rgb  #rgba  #rrggbb  #rrggbbaa   (CSS order: alpha last)
//   rgb(r,g,b)  rgba(r,g,b,a)
//   a fixed set of named colors (all map to constant values)
// -----------------------------------------------------------------------------

// Hard limits so a malicious/huge string can never blow up rendering.
var MAX_LENGTH = 4000;
var MAX_DECLARATIONS = 32;
var MAX_STOPS = 32;
var MAX_SHADOWS = 24;

var NAMED_COLORS = {
    "transparent": "#00000000",
    "black": "#000000",
    "white": "#ffffff",
    "red": "#ff0000",
    "green": "#008000",
    "blue": "#0000ff",
    "yellow": "#ffff00",
    "orange": "#ffa500",
    "pink": "#ffc0cb",
    "purple": "#800080",
    "cyan": "#00ffff",
    "aqua": "#00ffff",
    "magenta": "#ff00ff",
    "fuchsia": "#ff00ff",
    "lime": "#00ff00",
    "gray": "#808080",
    "grey": "#808080",
    "silver": "#c0c0c0",
    "gold": "#ffd700",
    "navy": "#000080",
    "teal": "#008080",
    "olive": "#808080",
    "maroon": "#800000"
};

function clamp(v, lo, hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

function pad2(n) {
    var s = n.toString(16);
    return s.length < 2 ? "0" + s : s;
}

// Build a QML color string. QML uses alpha-FIRST for 8-digit hex (#AARRGGBB).
function qmlColor(r, g, b, a) {
    r = clamp(Math.round(r), 0, 255);
    g = clamp(Math.round(g), 0, 255);
    b = clamp(Math.round(b), 0, 255);
    a = clamp(Math.round(a), 0, 255);
    if (a === 255)
        return "#" + pad2(r) + pad2(g) + pad2(b);
    return "#" + pad2(a) + pad2(r) + pad2(g) + pad2(b);
}

// Alpha byte (0..255) of a normalized QML color string produced by qmlColor().
function alphaOf(qmlColorStr) {
    if (typeof qmlColorStr !== "string")
        return 255;
    if (qmlColorStr.length === 9) // #AARRGGBB
        return parseInt(qmlColorStr.substring(1, 3), 16);
    return 255; // #RRGGBB is fully opaque
}

// Average RGB of a set of gradient stops, returned as an opaque QML hex color.
// Used as a solid "backing" fill so a gradient's glyph junctions stay filled.
function averageColor(stops) {
    if (!stops || stops.length === 0)
        return "#ffffff";
    var r = 0, g = 0, b = 0, n = 0;
    for (var i = 0; i < stops.length; ++i) {
        var h = ("" + stops[i].color).substring(1);
        var rr, gg, bb;
        if (h.length === 6) {
            rr = parseInt(h.substring(0, 2), 16);
            gg = parseInt(h.substring(2, 4), 16);
            bb = parseInt(h.substring(4, 6), 16);
        } else if (h.length === 8) {
            rr = parseInt(h.substring(2, 4), 16);
            gg = parseInt(h.substring(4, 6), 16);
            bb = parseInt(h.substring(6, 8), 16);
        } else {
            continue;
        }
        r += rr; g += gg; b += bb; ++n;
    }
    if (n === 0)
        return "#ffffff";
    return "#" + pad2(Math.round(r / n)) + pad2(Math.round(g / n)) + pad2(Math.round(b / n));
}

// Convert a normalized QML color string (#RRGGBB / #AARRGGBB) into a CSS
// "rgba(r,g,b,a)" string that the QtQuick Canvas 2D context understands.
function cssColor(qmlStr) {
    if (typeof qmlStr !== "string" || qmlStr.charAt(0) !== "#")
        return "rgba(0,0,0,0)";
    var h = qmlStr.substring(1);
    var r, g, b, a;
    if (h.length === 6) {
        r = parseInt(h.substring(0, 2), 16);
        g = parseInt(h.substring(2, 4), 16);
        b = parseInt(h.substring(4, 6), 16);
        a = 255;
    } else if (h.length === 8) {
        a = parseInt(h.substring(0, 2), 16);
        r = parseInt(h.substring(2, 4), 16);
        g = parseInt(h.substring(4, 6), 16);
        b = parseInt(h.substring(6, 8), 16);
    } else {
        return "rgba(0,0,0,0)";
    }
    return "rgba(" + r + "," + g + "," + b + "," + (a / 255) + ")";
}

// Parse one color token -> normalized QML color string, or null if invalid.
function parseColor(tokenRaw) {
    if (tokenRaw === undefined || tokenRaw === null)
        return null;
    var token = tokenRaw.trim().toLowerCase();
    if (token.length === 0)
        return null;

    if (NAMED_COLORS.hasOwnProperty(token))
        return NAMED_COLORS[token];

    // Hex.
    if (token.charAt(0) === "#") {
        var hex = token.substring(1);
        if (!/^[0-9a-f]+$/.test(hex))
            return null;
        var r, g, b, a;
        if (hex.length === 3) {
            r = parseInt(hex[0] + hex[0], 16);
            g = parseInt(hex[1] + hex[1], 16);
            b = parseInt(hex[2] + hex[2], 16);
            return qmlColor(r, g, b, 255);
        } else if (hex.length === 4) {
            r = parseInt(hex[0] + hex[0], 16);
            g = parseInt(hex[1] + hex[1], 16);
            b = parseInt(hex[2] + hex[2], 16);
            a = parseInt(hex[3] + hex[3], 16);
            return qmlColor(r, g, b, a);
        } else if (hex.length === 6) {
            r = parseInt(hex.substring(0, 2), 16);
            g = parseInt(hex.substring(2, 4), 16);
            b = parseInt(hex.substring(4, 6), 16);
            return qmlColor(r, g, b, 255);
        } else if (hex.length === 8) {
            r = parseInt(hex.substring(0, 2), 16);
            g = parseInt(hex.substring(2, 4), 16);
            b = parseInt(hex.substring(4, 6), 16);
            a = parseInt(hex.substring(6, 8), 16);
            return qmlColor(r, g, b, a);
        }
        return null;
    }

    // rgb() / rgba().
    var m = /^rgba?\(([^()]*)\)$/.exec(token);
    if (m) {
        var parts = m[1].split(",");
        if (parts.length !== 3 && parts.length !== 4)
            return null;
        var nums = [];
        for (var i = 0; i < parts.length; ++i) {
            var p = parts[i].trim();
            if (!/^[+]?\d*\.?\d+%?$/.test(p))
                return null;
            nums.push(p);
        }
        function channel(s) {
            if (s.charAt(s.length - 1) === "%")
                return parseFloat(s) * 255 / 100;
            return parseFloat(s);
        }
        var rr = channel(nums[0]);
        var gg = channel(nums[1]);
        var bb = channel(nums[2]);
        var aa = 255;
        if (nums.length === 4) {
            var av = nums[3];
            aa = (av.charAt(av.length - 1) === "%")
                    ? parseFloat(av) * 255 / 100
                    : parseFloat(av) * 255;
        }
        return qmlColor(rr, gg, bb, aa);
    }

    return null;
}

// Split by a delimiter char, but only at top nesting level (respect parens).
function splitTopLevel(str, delim) {
    var out = [];
    var depth = 0;
    var cur = "";
    for (var i = 0; i < str.length; ++i) {
        var c = str.charAt(i);
        if (c === "(") {
            depth++;
            cur += c;
        } else if (c === ")") {
            if (depth > 0)
                depth--;
            cur += c;
        } else if (c === delim && depth === 0) {
            out.push(cur);
            cur = "";
        } else {
            cur += c;
        }
    }
    out.push(cur);
    return out;
}

// Tokenize by whitespace at top level (respect parens so rgba(...) stays whole).
function splitWhitespaceTopLevel(str) {
    var out = [];
    var depth = 0;
    var cur = "";
    for (var i = 0; i < str.length; ++i) {
        var c = str.charAt(i);
        if (c === "(") { depth++; cur += c; }
        else if (c === ")") { if (depth > 0) depth--; cur += c; }
        else if ((c === " " || c === "\t" || c === "\n" || c === "\r") && depth === 0) {
            if (cur.length > 0) { out.push(cur); cur = ""; }
        } else {
            cur += c;
        }
    }
    if (cur.length > 0)
        out.push(cur);
    return out;
}

// "<number>px" or bare "<number>" -> pixels, or null if not a length.
function parseLength(token) {
    var t = token.trim().toLowerCase();
    if (/^[+-]?\d*\.?\d+px$/.test(t))
        return parseFloat(t);
    if (/^[+-]?\d*\.?\d+$/.test(t))
        return parseFloat(t);
    return null;
}

// "<number>%" or "<number>" (fraction 0..1 also accepted) -> 0..1, or null.
function parsePositionFraction(token) {
    var t = token.trim();
    if (/^[+-]?\d*\.?\d+%$/.test(t))
        return clamp(parseFloat(t) / 100, 0, 1);
    return null;
}

function fail(msg) {
    return {
        valid: false,
        styled: false,
        error: msg,
        useGradient: false,
        solidColor: "",
        gradientAngle: 180,
        gradientStops: [],
        shadows: []
    };
}

function empty() {
    return {
        valid: true,
        styled: false,
        error: "",
        useGradient: false,
        solidColor: "",
        gradientAngle: 180,
        gradientStops: [],
        shadows: []
    };
}

function parseLinearGradient(value) {
    // value already confirmed to start with "linear-gradient(".
    var open = value.indexOf("(");
    var close = value.lastIndexOf(")");
    if (open < 0 || close < 0 || close <= open)
        return null;
    var inner = value.substring(open + 1, close);
    var tokens = splitTopLevel(inner, ",");
    if (tokens.length < 2)
        return null;

    var angle = 180; // CSS default: "to bottom".
    var firstTrim = tokens[0].trim().toLowerCase();
    var startIndex = 0;
    if (/^[+-]?\d*\.?\d+deg$/.test(firstTrim)) {
        angle = parseFloat(firstTrim);
        startIndex = 1;
    } else if (firstTrim.indexOf("to ") === 0) {
        // Support the common keyword directions.
        var dirs = {
            "to top": 0, "to right": 90, "to bottom": 180, "to left": 270,
            "to top right": 45, "to right top": 45,
            "to bottom right": 135, "to right bottom": 135,
            "to bottom left": 225, "to left bottom": 225,
            "to top left": 315, "to left top": 315
        };
        if (!dirs.hasOwnProperty(firstTrim))
            return null;
        angle = dirs[firstTrim];
        startIndex = 1;
    }

    var stops = [];
    for (var i = startIndex; i < tokens.length; ++i) {
        var parts = splitWhitespaceTopLevel(tokens[i]);
        if (parts.length < 1 || parts.length > 2)
            return null;
        var color = parseColor(parts[0]);
        if (color === null)
            return null;
        var pos = null;
        if (parts.length === 2) {
            pos = parsePositionFraction(parts[1]);
            if (pos === null)
                return null;
        }
        stops.push({ color: color, pos: pos });
        if (stops.length > MAX_STOPS)
            return null;
    }
    if (stops.length < 2)
        return null;

    // Fill in missing positions (CSS-like): ends default to 0 and 1, interior
    // undefined stops are spread evenly between their defined neighbours.
    if (stops[0].pos === null)
        stops[0].pos = 0;
    if (stops[stops.length - 1].pos === null)
        stops[stops.length - 1].pos = 1;
    var lastDefined = 0;
    for (var k = 1; k < stops.length; ++k) {
        if (stops[k].pos !== null) {
            var gap = k - lastDefined;
            if (gap > 1) {
                var startP = stops[lastDefined].pos;
                var endP = stops[k].pos;
                for (var j = lastDefined + 1; j < k; ++j)
                    stops[j].pos = startP + (endP - startP) * (j - lastDefined) / gap;
            }
            lastDefined = k;
        }
    }
    // Enforce non-decreasing positions (CSS clamps each to the previous max).
    var maxSoFar = 0;
    for (var q = 0; q < stops.length; ++q) {
        if (stops[q].pos < maxSoFar)
            stops[q].pos = maxSoFar;
        else
            maxSoFar = stops[q].pos;
    }

    return { angle: angle, stops: stops };
}

function parseTextShadows(value) {
    var list = splitTopLevel(value, ",");
    var shadows = [];
    for (var i = 0; i < list.length; ++i) {
        var raw = list[i].trim();
        if (raw.length === 0)
            continue;
        var tokens = splitWhitespaceTopLevel(raw);
        var lengths = [];
        var color = null;
        for (var t = 0; t < tokens.length; ++t) {
            var lp = parseLength(tokens[t]);
            if (lp !== null) {
                lengths.push(lp);
                continue;
            }
            var cp = parseColor(tokens[t]);
            if (cp !== null) {
                if (color !== null)
                    return null; // two colors in one shadow -> invalid
                color = cp;
                continue;
            }
            return null; // unknown token
        }
        if (lengths.length < 2 || lengths.length > 3)
            return null;
        var dx = lengths[0];
        var dy = lengths[1];
        var blur = lengths.length >= 3 ? lengths[2] : 0;
        if (blur < 0)
            blur = 0;
        shadows.push({ dx: dx, dy: dy, blur: blur, color: color });
        if (shadows.length > MAX_SHADOWS)
            return null;
    }
    return shadows;
}

// Main entry point. Returns a validated, plain data object (see fail()/empty()).
function parse(cssRaw) {
    if (cssRaw === undefined || cssRaw === null)
        return empty();
    var css = ("" + cssRaw).trim();
    if (css.length === 0)
        return empty();
    if (css.length > MAX_LENGTH)
        return fail("style too long");

    var declarations = splitTopLevel(css, ";");
    if (declarations.length > MAX_DECLARATIONS)
        return fail("too many declarations");

    var colorProp = null;    // last "color:"
    var bgKind = "none";     // "none" | "solid" | "gradient"
    var bgSolid = null;
    var bgGradient = null;
    var clipText = false;
    var fillColor = null;    // -webkit-text-fill-color
    var shadows = [];

    for (var d = 0; d < declarations.length; ++d) {
        var decl = declarations[d].trim();
        if (decl.length === 0)
            continue;
        var colon = decl.indexOf(":");
        if (colon < 0)
            return fail("declaration without ':'");
        var prop = decl.substring(0, colon).trim().toLowerCase();
        var value = decl.substring(colon + 1).trim();
        if (prop.length === 0 || value.length === 0)
            return fail("empty property or value");

        switch (prop) {
        case "color":
            var cc = parseColor(value);
            if (cc === null)
                return fail("invalid color value");
            colorProp = cc;
            break;

        case "background":
        case "background-image":
            var v = value;
            // Tolerate the "<image> text" clip shorthand seen in some styles.
            if (/\stext$/i.test(v)) {
                clipText = true;
                v = v.substring(0, v.length - 4).trim();
            }
            // A background value may hold several comma-separated layers; use
            // the first one (the topmost paint) and ignore the rest.
            var layers = splitTopLevel(v, ",");
            var firstLayer = layers[0].trim();
            if (firstLayer.toLowerCase().indexOf("linear-gradient(") === 0) {
                var g = parseLinearGradient(firstLayer);
                if (g === null)
                    return fail("invalid linear-gradient");
                bgKind = "gradient";
                bgGradient = g;
            } else {
                // Solid color, possibly followed by a background-position token
                // ("#C6DEE3 100%") which we don't need for text fills.
                var solidTokens = splitWhitespaceTopLevel(firstLayer);
                var solid = solidTokens.length > 0 ? parseColor(solidTokens[0]) : null;
                if (solid === null)
                    return fail("invalid background value");
                bgKind = "solid";
                bgSolid = solid;
            }
            break;

        case "background-clip":
        case "-webkit-background-clip":
            var cv = value.toLowerCase();
            if (cv === "text")
                clipText = true;
            else if (cv === "border-box" || cv === "padding-box" || cv === "content-box")
                clipText = false;
            else
                return fail("invalid background-clip value");
            break;

        case "-webkit-text-fill-color":
        case "text-fill-color":
            var fc = parseColor(value);
            if (fc === null)
                return fail("invalid text-fill-color value");
            fillColor = fc;
            break;

        case "text-shadow":
            var sh = parseTextShadows(value);
            if (sh === null)
                return fail("invalid text-shadow value");
            shadows = sh;
            break;

        default:
            return fail("unsupported property: " + prop);
        }
    }

    // Resolve which paint wins for the glyphs.
    var useGradient = false;
    var solidColor = "";
    var gradientAngle = 180;
    var gradientStops = [];

    var TRANSPARENT = "#00000000";
    // A mostly-opaque text-fill-color paints the glyphs directly; a low-alpha
    // one (like #ffffff1a) is meant to let a clipped gradient show through.
    var fillOpaque = fillColor !== null && alphaOf(fillColor) >= 250;
    var fillVisible = fillColor !== null && alphaOf(fillColor) > 0;

    if (fillOpaque) {
        solidColor = fillColor;
    } else if (clipText && bgKind === "gradient") {
        useGradient = true;
        gradientAngle = bgGradient.angle;
        gradientStops = bgGradient.stops;
    } else if (fillVisible) {
        solidColor = fillColor;
    } else if (clipText && bgKind === "solid" && bgSolid !== TRANSPARENT) {
        solidColor = bgSolid;
    } else if (colorProp !== null && colorProp !== TRANSPARENT) {
        solidColor = colorProp;
    } else {
        solidColor = ""; // renderer falls back to the theme color
    }

    var styled = useGradient || solidColor.length > 0 || shadows.length > 0;

    return {
        valid: true,
        styled: styled,
        error: "",
        useGradient: useGradient,
        solidColor: solidColor,
        gradientAngle: gradientAngle,
        gradientStops: gradientStops,
        shadows: shadows
    };
}

function isValid(css) {
    return parse(css).valid;
}
