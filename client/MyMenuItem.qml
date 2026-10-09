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

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }

    property bool leftAlign: false
    property bool isRed: false
    property string hotKey: ""
    property bool hotKeyVisible: true
    property string iconSource: "resources/settings.svg"
    property color colorBackground: theme.background
    property color colorHighlighted: theme.lowestAccent
    property color colorRedAccent: theme.redAccent
    property color colorAccent: theme.accent

    // Measure the label (and optional hotkey) so the item — and therefore the
    // whole Menu — grows to fit its text in any locale instead of clipping.
    // Longer translations (e.g. RU "Информация о контакте") previously overflowed
    // the hard-coded 150px width. 150 stays as the minimum.
    TextMetrics {
        id: labelMetrics
        font.family: geologicaFont.name
        font.pixelSize: 12
        text: control.text
    }
    TextMetrics {
        id: hotKeyMetrics
        font.family: geologicaFont.name
        font.pixelSize: 10
        text: (control.hotKeyVisible && control.hotKey.length > 0) ? control.hotKey : ""
    }

    readonly property int contentPadding: 10  // RowLayout anchors.margins (5 + 5)
    readonly property int iconBlockWidth: control.iconSource.length > 0
        ? (15 + 18 + 5)                       // leftMargin + icon + row spacing
        : (control.leftAlign ? 15 : 0)        // text indent when there is no icon
    readonly property int hotKeyBlockWidth: hotKeyMetrics.text.length > 0
        ? (5 + Math.ceil(hotKeyMetrics.advanceWidth) + 10)
        : 10                                  // trailing breathing room

    implicitWidth: Math.max(150, contentPadding + iconBlockWidth
        + Math.ceil(labelMetrics.advanceWidth) + hotKeyBlockWidth
        + (control.leftAlign ? 0 : 10)        // right-aligned label rightMargin
        + 16)                                 // comfort margin so text never hugs the edge
    implicitHeight: 45

    focus: false

    contentItem: RowLayout {
        anchors.fill: parent
        anchors.margins: 5

        ColorImage {
            source: control.iconSource
            accentColor: control.isRed ? control.colorRedAccent : control.colorAccent
            visible: control.iconSource.length > 0
            Layout.alignment: Qt.AlignVCenter
            Layout.leftMargin: 15
            Layout.preferredWidth: 18
            Layout.preferredHeight: 18
        }

        Text {
            text: control.text
            font.family: geologicaFont.name
            font.pixelSize: 12
            color: isRed ? control.colorRedAccent : (control.highlighted ? control.colorAccent : control.colorAccent)
            horizontalAlignment: leftAlign ? Text.AlignLeft : Text.AlignRight
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter

            Layout.leftMargin: leftAlign ? (control.iconSource !== "" ? 0 : 15) : 0
            Layout.rightMargin: leftAlign ? 0 : 10
        }

        Text {
            text: control.hotKey
            font.family: geologicaFont.name
            font.pixelSize: 10
            color: !control.hotKeyVisible ? "transparent" : (isRed ? control.colorRedAccent : (control.highlighted ? control.colorAccent : Qt.darker(control.colorAccent, 1.2)))
            horizontalAlignment: Text.AlignRight
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

            Layout.rightMargin: 10
        }
    }

    background: Rectangle {
        // Transparent when not highlighted so the menu's elevated
        // chatPanelSurface shows through; hover = that surface lifted ~3%
        // toward white (a subtle brighten), applied as a translucent white
        // overlay so it works on every theme — the old colorHighlighted
        // (adjustContrast of the background) went black on dark themes.
        color: control.highlighted ? Qt.rgba(1, 1, 1, 0.03) : "transparent"
        radius: 8
        anchors.fill: parent
        anchors.margins: 2
        anchors.topMargin: 1
        anchors.bottomMargin: 1
    }
}