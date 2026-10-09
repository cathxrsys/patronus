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
    property real effectMaskThresholdMin: 0.5

    implicitWidth: 18
    implicitHeight: 18

    Image {
        id: iconImage
        anchors.fill: parent
        mipmap: root.mipmap
        visible: false

        source: "resources/logo.svg"

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