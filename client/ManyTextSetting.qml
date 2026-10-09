import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material

// Multi-line labelled text field (e.g. "About Me"). Same visual language as
// TextSetting: label above a bordered box that grows with its content and
// highlights on focus.
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
    // the PanelBorder rim and the same (capped) pill radius as the message input.
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
        // Grows with the content, but never shorter than a comfortable minimum.
        Layout.preferredHeight: Math.max(88, settingInput.contentHeight + settingInput.topPadding + settingInput.bottomPadding + 2)

        // Match the chat message composer: capped pill radius, a 2% surface lift
        // and a PanelBorder rim. A crisp accent ring is layered on only on focus.
        radius: Math.min(height / 2, 22)

        color: root.panelSurfaceColor
        border.width: settingInput.activeFocus ? 1 : 0
        border.color: settingInput.activeFocus ? Qt.darker(root.accentColor, 2.0) : "transparent"

        Behavior on border.color { ColorAnimation { duration: 130 } }

        PanelBorder {
            edgeColor: root.panelBorderColor
        }

        TextArea {
            id: settingInput
            anchors.fill: parent

            // Seed once, imperatively — see the note in TextSetting.qml. A live
            // `text:` binding gets clobbered to empty by the Android IME on focus.
            Component.onCompleted: settingInput.text = Settings.getTextSetting(settingName, defaultValue)
            placeholderText: root.placeholderText
            placeholderTextColor: root.lowAccentColor

            leftPadding: 12
            rightPadding: 12
            topPadding: 10
            bottomPadding: 10

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
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            clip: false

            background: Rectangle { color: "transparent" }

            Material.foreground: accentColor
            Material.accent: accentColor

            // NOTE: no ImhHiddenText — this is a plain multi-line text field, not a
            // password. Password mode makes the Android IME drop the field's existing
            // text on focus (the "tap → name becomes Anonymous" bug).
            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhSensitiveData
        }
    }
}
