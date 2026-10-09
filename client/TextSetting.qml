import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

// Single-line labelled text field used across the settings page.
// The label sits above a clearly-bordered input box that lights up on focus,
// so it reads unmistakably as "something you can type into".
ColumnLayout {
    id: root

    ThemeDark {
        id: theme
    }

    property color accentColor: "white"
    property color lowAccentColor: "lightgray"
    property color lowestAccentColor: "lightgray"
    property color backgroundColor: theme.background
    property string settingText: "Setting Text"
    property string settingName: "settingName"
    property string defaultValue: ""
    property string placeholderText: ""
    property string inputText: settingInput.text

    // Chat-composer look: a barely-there 1% lift off the background, delineated by
    // the PanelBorder rim and the same pill radius as the message input.
    property color panelSurfaceColor: Qt.tint(backgroundColor, Qt.rgba(1, 1, 1, 0.01))
    property color panelBorderColor: Qt.rgba(accentColor.r, accentColor.g, accentColor.b, 0.10)

    Layout.fillWidth: true
    // A little breathing room above each styled field so stacked inputs don't crowd.
    Layout.topMargin: 14

    spacing: 6

    Text {
        text: settingText
        font.pixelSize: 14
        color: accentColor
        font.family: geologicaFont.name
        font.weight: Font.Light
        Layout.fillWidth: true
    }

    Rectangle {
        id: inputContainer

        Layout.fillWidth: true
        Layout.preferredHeight: 44

        // Match the chat message composer: pill radius, a 2% surface lift and a
        // PanelBorder rim. A crisp accent ring is layered on only while focused.
        radius: Math.min(height / 2, 22)

        color: root.panelSurfaceColor
        border.width: settingInput.activeFocus ? 1 : 0
        border.color: settingInput.activeFocus ? Qt.darker(root.accentColor, 2.0) : "transparent"

        Behavior on border.color { ColorAnimation { duration: 130 } }

        PanelBorder {
            edgeColor: root.panelBorderColor
        }

        TextField {
            id: settingInput
            anchors.fill: parent

            // Seed once, imperatively. A live `text:` binding to a stored value
            // fights the Android IME: when the keyboard attaches on focus, Qt
            // re-syncs the field and clobbers the binding to empty — that is the
            // "tap the name field → name becomes Anonymous" bug. Assigning on
            // completion (no live binding) leaves nothing for the IME to clobber.
            Component.onCompleted: settingInput.text = Settings.getTextSetting(settingName, defaultValue)
            placeholderText: root.placeholderText
            placeholderTextColor: root.lowAccentColor

            leftPadding: 12
            rightPadding: 12
            topPadding: 0
            bottomPadding: 0
            verticalAlignment: TextInput.AlignVCenter
            horizontalAlignment: Text.AlignLeft

            onActiveFocusChanged: {
                if (activeFocus && Platform.isAndroid && AndroidSystemUi.available) {
                    Qt.callLater(function() {
                        if (settingInput.activeFocus) {
                            settingInput.Window.window.lastKeyboardTarget = settingInput
                            AndroidSystemUi.showKeyboard()
                        }
                    })
                }
            }

            selectionColor: root.accentColor
            selectedTextColor: root.backgroundColor

            font.pixelSize: 14
            font.family: "Roboto"
            font.weight: Font.Light

            color: accentColor
            background: Rectangle { color: "transparent" }

            Material.foreground: accentColor
            Material.accent: accentColor

            // NOTE: no ImhHiddenText — this is a plain name/text field, not a
            // password. Password mode makes the Android IME drop the field's
            // existing text on focus (the "tap → name becomes Anonymous" bug).
            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhSensitiveData
        }
    }
}
