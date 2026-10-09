import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material
import QtQuick.Effects



Button {
    ThemeDark {
        id: theme
    }

    id: root

    property color buttonColor: theme.accent
    property color textColor: theme.background

    text: "button"
    font.pixelSize: 12
    font.weight: Font.Medium

    anchors.leftMargin: 0
    anchors.rightMargin: 0
    anchors.topMargin: 0
    anchors.bottomMargin: 0

    Material.background: root.buttonColor

    Material.roundedScale: Material.NotRounded

    Material.foreground: root.textColor

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }

    height: 40
}