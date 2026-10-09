import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls.Material
import QtQuick.Effects
import QtQuick.Shapes

RowLayout {
    property color accentColor: "white"
    property string settingText: "Setting Text"
    property string settingName: "settingName"
    property bool defaultValue: true
    property real maximumContentWidth: 400
    readonly property real resolvedWidth: Math.min(maximumContentWidth, parent && parent.width > 0 ? parent.width : maximumContentWidth)

    Layout.preferredWidth: resolvedWidth
    Layout.maximumWidth: resolvedWidth
    Layout.minimumWidth: resolvedWidth
    Layout.preferredHeight: 30

    Text {
        text: settingText
        font.pixelSize: 14
        color: accentColor
        Layout.alignment: Qt.AlignLeft
        Layout.fillWidth: true
        font.family: geologicaFont.name
        font.weight: Font.Light
        rightPadding: 10
    }

    Rectangle {
        Layout.preferredWidth: 40
        Layout.maximumWidth: 40
        Layout.minimumWidth: 40

        Layout.preferredHeight: 16
        Layout.maximumHeight: 16
        Layout.minimumHeight: 16

        color: "transparent"

        Layout.alignment: Qt.AlignRight
        Switch {
            anchors.centerIn: parent
            anchors.fill: parent
            checked: Settings.getBoolSetting(settingName, defaultValue)
            icon.color: accentColor
            scale: 0.7

            onCheckedChanged: {
                Settings.setBoolSetting(settingName, checked)
            }
        }
    }
}