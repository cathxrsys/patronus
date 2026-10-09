import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts


MenuItem {
    id: control

    FontLoader {
        id: geologicaFont
        source: "resources/fonts/geologica.ttf"
    }

    ThemeDark {
        id: theme
    }

    property bool leftAlign: false
    property bool isRed: false
    property string hotKey: ""
    property string iconsource: ""
    property color backgroundColor: theme.background
    property color accentColor: theme.accent
    property color lowestAccentColor: theme.lowestAccent

    implicitWidth: 150
    implicitHeight: 30

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }

    focus: false

    contentItem: RowLayout {
        anchors.fill: parent
        anchors.margins: 5

        Image {
            source: control.iconsource
            visible: control.iconsource.length > 0
            Layout.alignment: Qt.AlignVCenter
            Layout.leftMargin: 8
            Layout.preferredWidth: 14
            Layout.preferredHeight: 14
            fillMode: Image.PreserveAspectFit
            mipmap: true
            smooth: true
            antialiasing: true
        }

        Text {
            text: control.text
            font.family: geologicaFont.name
            font.pixelSize: 12
            color: isRed ? theme.redAccent : (control.highlighted ? root.accentColor : root.lowestAccentColor)
            //horizontalAlignment: leftAlign ? Text.AlignLeft : Text.AlignRight
            Layout.alignment: leftAlign ? Qt.AlignLeft | Qt.AlignVCenter : Qt.AlignRight | Qt.AlignVCenter

            Layout.leftMargin: leftAlign ? 10 : 0
            Layout.rightMargin: leftAlign ? 0 : 10
        }

        Text {
            text: control.hotKey
            font.family: geologicaFont.name
            font.pixelSize: 10
            color: isRed ? theme.redAccent : (control.highlighted ? root.accentColor : root.lowestAccentColor)
            horizontalAlignment: Text.AlignRight
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

            Layout.rightMargin: 10
        }
    }

    background: Rectangle {
        // Transparent base lets the menu's elevated chatPanelSurface show through;
        // hover brightens the surface ~3% via a theme-agnostic white overlay.
        color: control.highlighted ? Qt.rgba(1, 1, 1, 0.03) : "transparent"
        radius: 8
        anchors.fill: parent
        anchors.margins: 2
        anchors.topMargin: 1
        anchors.bottomMargin: 1
    }
}