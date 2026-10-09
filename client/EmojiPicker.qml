import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material
import "EmojiData.js" as EmojiData

// Emoji picker popup shown above the composer's smile button.
//
// Renders the bundled Unicode set (EmojiData.js, generated from Unicode's
// emoji-test.txt ∩ NotoColorEmoji coverage) grouped into the standard nine
// categories, plus a synthetic "recent" group persisted via Settings. Emits
// `picked(emoji)` — the composer inserts it at the caret.
//
// Relies on ids/properties resolved from Main.qml's context (theme,
// emojiFont, geologicaFont, Settings, Platform, mainWindow), the same
// dynamic-scoping pattern BoolSetting/TextSetting already use.
Popup {
    id: root

    signal picked(string emoji)

    padding: 0
    modal: false
    // Never takes focus — stealing it would drop the composer's soft keyboard.
    // The composer's caret (cursorPosition) persists without focus, so inserts
    // still land correctly. Tapping an emoji stays inside the popup, so it
    // doesn't self-close.
    focus: false
    closePolicy: Popup.CloseOnPressOutsideParent | Popup.CloseOnEscape

    readonly property int pad: 8
    readonly property int minCell: Platform.isMobile ? 44 : 38
    // Keeps the panel off the window edges. On mobile Main.qml already insets the
    // panel 8px on each side (x: 8, width: parent.width - 16), so here this mainly
    // holds the 8px gap above the bottom nav bar; on desktop it clamps the floating
    // bubble inside the window. (Must agree with the mobile x/width, or a popup too
    // wide for the margin gets shoved off the right edge.)
    margins: 8
    width: Platform.isMobile ? Math.min(mainWindow.width - 16, 340) : 348
    height: Platform.isMobile ? 260 : 296

    // ---- recents (stored as a single space-separated string) ----
    property var recent: []
    function loadRecent() {
        var s = Settings.getTextSetting("recentEmoji", "")
        var parts = s.length > 0 ? s.split(" ") : []
        var out = []
        for (var i = 0; i < parts.length; i++)
            if (parts[i].length > 0)
                out.push(parts[i])
        recent = out
    }
    function pushRecent(e) {
        var arr = recent.slice()
        var idx = arr.indexOf(e)
        if (idx !== -1)
            arr.splice(idx, 1)
        arr.unshift(e)
        if (arr.length > 32)
            arr = arr.slice(0, 32)
        recent = arr
        Settings.setTextSetting("recentEmoji", arr.join(" "))
    }

    // ---- current view (one category at a time) ----
    readonly property var cats: EmojiData.categories
    property string currentKey: "recent"

    function listFor(key) {
        if (key === "recent") {
            var r = []
            for (var k = 0; k < recent.length; k++)
                r.push({ e: recent[k], n: "" })
            return r
        }
        for (var c = 0; c < cats.length; c++)
            if (cats[c].key === key)
                return cats[c].emoji
        return []
    }
    property var currentList: listFor(currentKey)
    onCurrentKeyChanged: currentList = listFor(currentKey)
    // Keep the grid live when picking while the recents tab is open.
    onRecentChanged: if (currentKey === "recent")
                         currentList = listFor(currentKey)

    onAboutToShow: {
        loadRecent()
        currentKey = recent.length > 0 ? "recent" : "smileys"
        currentList = listFor(currentKey)
    }

    background: Rectangle {
        radius: 12
        // Same treatment as the composer pill: background+1% brightness, plus
        // the tapered PanelBorder stroke (see PanelBorder.qml) instead of a
        // flat uniform border.
        color: mainWindow.mixColors(theme.background, Qt.rgba(1, 1, 1, 1), 0.01)

        PanelBorder {
            edgeColor: mainWindow.chatPanelBorderColor
        }
    }

    contentItem: ColumnLayout {
        spacing: 6

        // ---- grid ----
        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: root.pad
            Layout.leftMargin: root.pad
            Layout.rightMargin: root.pad
            clip: true
            model: root.currentList

            readonly property int columns: Math.max(6, Math.floor(width / root.minCell))
            cellWidth: Math.floor(width / columns)
            cellHeight: cellWidth
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            // Empty state (mostly: recents tab before anything was picked).
            Text {
                anchors.centerIn: parent
                width: parent.width - 24
                visible: grid.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("No recent emoji yet")
                color: theme.lowAccent
                font.family: geologicaFont.name
                font.pixelSize: 12
            }

            delegate: Item {
                width: grid.cellWidth
                height: grid.cellHeight

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    radius: 7
                    color: cellHover.hovered ? mainWindow.mixColors(theme.background, theme.accent, 0.14)
                                             : "transparent"
                }
                Text {
                    anchors.centerIn: parent
                    text: modelData.e
                    font.family: emojiFont.name
                    font.pixelSize: Math.round(grid.cellWidth * 0.56)
                }
                HoverHandler { id: cellHover; enabled: !Platform.isMobile }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        // picked() first: pushRecent() mutates `recent`, which rebuilds
                        // `currentList` (a new array) whenever the recents tab is showing —
                        // GridView treats that as a brand-new model and recreates its
                        // delegates, including this one, mid-click. Reading `modelData`
                        // AFTER that swap landed on the wrong emoji (needed a second tap).
                        var e = modelData.e
                        root.picked(e)
                        root.pushRecent(e)
                    }
                }
            }
        }

        // ---- category tab strip ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: mainWindow.chatPanelBorderColor
            opacity: 0.6
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: root.pad / 2
            Layout.rightMargin: root.pad / 2
            Layout.bottomMargin: 4
            spacing: 0

            // recent first, then the nine categories
            Repeater {
                model: [{ key: "recent", icon: "🕒" }].concat(root.cats)
                delegate: Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Platform.isMobile ? 34 : 30
                    readonly property bool active: root.currentKey === modelData.key

                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.height - 4
                        height: width
                        radius: 6
                        visible: parent.active || tabHover.hovered
                        color: parent.active ? mainWindow.mixColors(theme.background, theme.accent, 0.18)
                                             : mainWindow.mixColors(theme.background, Qt.rgba(1, 1, 1, 1), 0.05)
                    }
                    Text {
                        anchors.centerIn: parent
                        text: modelData.icon
                        font.family: emojiFont.name
                        font.pixelSize: Platform.isMobile ? 18 : 15
                        opacity: parent.active ? 1.0 : 0.72
                    }
                    HoverHandler { id: tabHover; enabled: !Platform.isMobile }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.currentKey = modelData.key
                            grid.positionViewAtBeginning()
                        }
                    }
                }
            }
        }
    }
}
