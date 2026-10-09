import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Material
import "NameStyleParser.js" as NSP
import "NameStylePresets.js" as Presets

// -----------------------------------------------------------------------------
// NameStyleField
//
// Self-contained editor for a "unique name style": a preset picker, an editable
// CSS field (restricted subset), a live validity hint and a live preview of the
// user's own "First Last" with the style applied.
//
// Read `styleCss` to get the current value to save. `valid` tells whether the
// current text passes the strict parser (invalid styles are simply ignored when
// rendering, but the hint lets the user know).
// -----------------------------------------------------------------------------
ColumnLayout {
    id: root

    FontLoader {
        id: geologicaFont
        source: "resources/fonts/geologica.ttf"
    }
    ThemeDark {
        id: theme
    }

    property color accentColor: theme.accent
    property color lowAccentColor: theme.lowAccent
    property color lowestAccentColor: theme.lowestAccent
    property color backgroundColor: theme.background

    property string settingName: "nameStyle"
    property string previewFirstName: "Ivan"
    property string previewLastName: "Ivanov"

    // Raw-CSS editing is for people who know CSS, so it stays collapsed by
    // default; everyone else just picks a preset. Toggled from the "Style" header.
    property bool cssExpanded: false

    // Public outputs.
    property alias styleCss: cssArea.text
    readonly property bool valid: NSP.isValid(cssArea.text)

    Layout.fillWidth: true
    spacing: 8

    // Label describing what the currently-typed style resolves to.
    function currentPresetLabel() {
        var t = cssArea.text.trim()
        if (t.length === 0)
            return qsTr("Default (no style)")
        for (var i = 0; i < Presets.list.length; ++i) {
            if (Presets.list[i].css.trim() === t)
                return Presets.list[i].name
        }
        return qsTr("Custom")
    }

    Text {
        text: qsTr("Preset")
        font.pixelSize: 14
        color: root.accentColor
        font.family: geologicaFont.name
        font.weight: Font.Light
        Layout.fillWidth: true
    }

    // ---- preset picker -----------------------------------------------------
    Rectangle {
        id: pickerButton
        Layout.fillWidth: true
        Layout.preferredHeight: 36
        color: "transparent"
        border.color: root.lowestAccentColor
        border.width: 1
        radius: 4

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12

            Text {
                text: root.currentPresetLabel()
                color: root.accentColor
                font.pixelSize: 13
                font.family: geologicaFont.name
                elide: Text.ElideRight
                Layout.fillWidth: true
                verticalAlignment: Text.AlignVCenter
            }
            ColorImage {
                // An SVG icon rather than a Unicode glyph, which some mobile
                // fonts render as a missing-character box.
                source: "resources/arrow-down.svg"
                accentColor: root.accentColor
                Layout.preferredWidth: 12
                Layout.preferredHeight: 12
                Layout.alignment: Qt.AlignVCenter
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: presetMenu.open()
        }

        Menu {
            id: presetMenu
            y: pickerButton.height
            topInset: 0
            bottomInset: 0
            padding: 0
            width: pickerButton.width
            // Cap the height so a long preset list stays compact and scrolls
            // (the menu's list view flicks/scrolls when items overflow).
            height: Math.min(implicitHeight, 300)

            background: Rectangle {
                color: root.backgroundColor
                border.color: root.lowestAccentColor
                radius: 4
            }

            MyMenuItem {
                text: qsTr("Default (no style)")
                iconSource: ""
                leftAlign: true
                width: pickerButton.width
                colorAccent: root.accentColor
                colorBackground: root.backgroundColor
                colorHighlighted: Qt.lighter(root.backgroundColor, 1.3)
                onTriggered: cssArea.text = ""
            }

            Instantiator {
                model: Presets.list
                // Each preset is shown rendered in its own style.
                delegate: MenuItem {
                    id: presetItem
                    required property var modelData
                    width: pickerButton.width
                    height: 42
                    padding: 0
                    leftPadding: 12
                    rightPadding: 12

                    background: Rectangle {
                        color: presetItem.highlighted
                               ? Qt.lighter(root.backgroundColor, 1.3)
                               : root.backgroundColor
                    }

                    contentItem: StyledName {
                        firstName: presetItem.modelData.name
                        lastName: ""
                        styleCss: presetItem.modelData.css
                        fallbackColor: root.accentColor
                        pixelSize: 15
                        fontFamily: geologicaFont.name
                        fontWeight: Font.Medium
                        horizontalAlignment: Text.AlignLeft
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }

                    onTriggered: cssArea.text = presetItem.modelData.css
                }
                onObjectAdded: (index, object) => presetMenu.insertItem(index + 1, object)
                onObjectRemoved: (index, object) => presetMenu.removeItem(object)
            }
        }
    }

    // ---- editable CSS (advanced, collapsed by default) ---------------------
    // Clickable header that expands/collapses the raw-CSS editor below.
    Item {
        id: cssHeader
        Layout.fillWidth: true
        Layout.topMargin: 4
        implicitHeight: cssHeaderRow.implicitHeight

        RowLayout {
            id: cssHeaderRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            Text {
                text: qsTr("Style")
                font.pixelSize: 14
                color: root.accentColor
                font.family: geologicaFont.name
                font.weight: Font.Light
            }
            Text {
                text: qsTr("(CSS — for advanced users)")
                font.pixelSize: 11
                color: root.lowAccentColor
                font.family: geologicaFont.name
                font.weight: Font.Light
                verticalAlignment: Text.AlignVCenter
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            ColorImage {
                source: "resources/arrow-down.svg"
                accentColor: root.accentColor
                Layout.preferredWidth: 12
                Layout.preferredHeight: 12
                Layout.alignment: Qt.AlignVCenter
                rotation: root.cssExpanded ? 180 : 0
                Behavior on rotation { NumberAnimation { duration: 120 } }
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.cssExpanded = !root.cssExpanded
        }
    }

    Rectangle {
        visible: root.cssExpanded
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(70, cssArea.contentHeight + cssArea.topPadding + cssArea.bottomPadding + 2)
        color: "transparent"
        border.color: cssArea.text.length > 0 && !root.valid ? theme.redAccent : root.lowestAccentColor
        border.width: 1
        radius: 4

        TextArea {
            id: cssArea
            anchors.fill: parent
            text: Settings.getTextSetting(root.settingName, "")
            leftPadding: 10
            rightPadding: 10
            topPadding: 8
            bottomPadding: 8
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            font.pixelSize: 12
            font.family: "Roboto"
            color: root.accentColor
            selectedTextColor: root.backgroundColor
            Material.foreground: root.accentColor
            Material.background: root.lowAccentColor
            background: Rectangle { color: "transparent" }
            // No predictive text: this is code, not prose.
            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            onActiveFocusChanged: {
                if (activeFocus && Platform.isAndroid && AndroidSystemUi.available) {
                    Qt.callLater(function() {
                        if (cssArea.activeFocus) {
                            cssArea.Window.window.lastKeyboardTarget = cssArea
                            AndroidSystemUi.showKeyboard()
                        }
                    })
                }
            }
        }
    }

    Text {
        Layout.fillWidth: true
        font.pixelSize: 11
        font.family: geologicaFont.name
        visible: root.cssExpanded && cssArea.text.length > 0
        text: root.valid ? qsTr("Style is valid") : qsTr("Invalid style — it will not be applied")
        color: root.valid ? root.accentColor : theme.redAccent
    }

    // ---- live preview ------------------------------------------------------
    Text {
        text: qsTr("Preview")
        font.pixelSize: 14
        color: root.accentColor
        font.family: geologicaFont.name
        font.weight: Font.Light
        Layout.fillWidth: true
        Layout.topMargin: 4
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 64
        radius: 4
        color: root.backgroundColor
        border.color: root.lowestAccentColor
        border.width: 1

        StyledName {
            anchors.centerIn: parent
            width: parent.width - 24
            firstName: root.previewFirstName.length > 0 ? root.previewFirstName : "Anonymous"
            lastName: root.previewLastName
            styleCss: cssArea.text
            fallbackColor: root.accentColor
            pixelSize: 24
            fontFamily: geologicaFont.name
            fontWeight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
    }
}
