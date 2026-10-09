import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls.Material
import QtQuick.Effects
import QtQuick.Shapes

ColumnLayout {
    property color accentColor: "white"
    property color textColor: accentColor
    property string groupText: "Setting Text"

    Layout.preferredHeight: 40

    Layout.topMargin: 20

    RowLayout {
        Layout.alignment: Qt.AlignTop
        Layout.fillHeight: true

        Text {
            text: groupText
            font.pixelSize: 16
            color: textColor
            Layout.alignment: Qt.AlignLeft
            Layout.fillWidth: true
            font.family: geologicaFont.name
            font.weight: Font.Light
            rightPadding: 10
            bottomPadding: 5
        } // Text

        // Rectangle {
        //     color: "transparent"
        //     Layout.preferredWidth: 20
        //     Layout.maximumWidth: 20
        //     Layout.minimumWidth: 20
        //     Layout.fillHeight: true
        // } // Rectangle
    } // RowLayout

    GradientLine {
        colorSide: "transparent"
        colorOutSide: accentColor
        colorCenter: accentColor

        Layout.preferredHeight: 1
        Layout.maximumHeight: 1
        Layout.minimumHeight: 1

        Layout.fillWidth: true
        Layout.rightMargin: 20

        Layout.alignment: Qt.AlignBottom
        
        Layout.bottomMargin: 10

        gradient: Gradient {
            GradientStop { position: 0.0; color: accentColor }
            GradientStop { position: 1.0; color: "transparent" }

            orientation: Gradient.Horizontal
        } 
    } // GradientLine
} // ColumnLayout