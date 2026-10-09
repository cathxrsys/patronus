import QtQuick
import QtQuick.Effects

Item {
    id: root
    
    property alias source: iconImage.source
    property color accentColor: "white"

    property bool mipmap: true
    property bool smoothRendering: true
    property bool antialiasingRendering: true

    property real effectMaskSpreadAtMin: 1.0
    property real effectMaskThresholdMin: 0.6

    implicitWidth: 18
    implicitHeight: 18

    Image {
        id: iconImage
        anchors.fill: parent
        // Mipmapping a source rasterized straight to the item's own (small)
        // sourceSize below buys nothing and on some Android GPUs corrupts the
        // downsample of the larger-viewBox icons (settings/donate/hacker.svg
        // declare 512-1920px), so it's forced off here regardless of root.mipmap.
        mipmap: false
        visible: false

        // Without this, SVGs get rasterized at their own native/viewBox size
        // (some icons here declare viewBox up to 1920x1920) before being
        // downscaled to the item's actual size. On some Android GPUs that
        // oversized intermediate texture silently fails to render inside the
        // MultiEffect mask below, leaving the icon blank instead of just soft.
        sourceSize.width: Math.max(root.width, 1) * 2
        sourceSize.height: Math.max(root.height, 1) * 2

        layer.enabled: true
        layer.smooth: root.smoothRendering

        smooth: root.smoothRendering
        antialiasing: root.antialiasingRendering
    }

    Rectangle {
        id: colorOverlay
        anchors.fill: iconImage
        color: root.accentColor
        visible: false

        layer.enabled: true
        layer.smooth: root.smoothRendering
    }

    MultiEffect {
        id: iconEffect
        anchors.fill: parent

        source: colorOverlay

        maskEnabled: true
        maskSource: iconImage

        visible: root.source !== ""

        layer.enabled: true
        layer.smooth: root.smoothRendering

        maskSpreadAtMin: root.effectMaskSpreadAtMin
        maskThresholdMin: root.effectMaskThresholdMin
    }
}