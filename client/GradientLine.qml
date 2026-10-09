import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls.Material
import QtQuick.Effects
import QtQuick.Shapes

Rectangle {
    id: root

    property color colorSide: "transparent"
    property color colorOutSide: "transparent"
    property color colorCenter: "transparent"

    height: 1
    radius: 0

    color: "transparent"

    layer.enabled: true

    gradient: Gradient {
        GradientStop { position: 0.0; color: root.colorSide }
        GradientStop { position: 0.4; color: root.colorOutSide }
        GradientStop { position: 0.5; color: root.colorCenter }
        GradientStop { position: 0.6; color: root.colorOutSide }
        GradientStop { position: 1.0; color: root.colorSide }

        orientation: Gradient.Horizontal

    }
}