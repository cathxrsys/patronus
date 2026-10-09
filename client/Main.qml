import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls.Material
import QtQuick.Effects
import QtQuick.Shapes
import QtQuick.Dialogs
import QtQml
import QtMultimedia
import Clipboard 1.0
import CoreCryptoAdapter 1.0
import App.Signals 1.0
import App.Ui 1.0
import Qr 1.0

import "qrcode.js" as QRLib

Window {
    id: mainWindow

    // Tracks the last QML text item that triggered showKeyboard(). Used in
    // onKeyboardHeightChanged handlers as fallback when activeFocusItem is null
    // (can happen when our IC-init fallback calls requestFocus(qtEditText)).
    property var lastKeyboardTarget: null

    // Height the mobile emoji picker currently reserves at the bottom of the
    // screen (0 when closed/desktop). Added into `stack`'s bottomMargin below
    // so it occupies the same space as the real IME — see emojiButton.
    property real emojiPanelHeight: 0
    // Most recent non-zero AndroidSystemUi.keyboardHeight seen this session,
    // so the panel can match the keyboard's own height even if opened before
    // the keyboard has ever been shown (first tap of a fresh session).
    property int lastKnownKeyboardHeight: 0
    // Set around the emoji panel's opening jump so `stack`'s bottomMargin
    // Behavior is skipped for that one instant change (see emojiButton).
    property bool suppressBottomMarginAnimation: false

    // Servers currently replaying their offline backlog (between MainSignals'
    // syncStarted and syncEnded). While any is syncing we suppress "new message"
    // tray notifications: those messages are history being delivered right after
    // (re)connecting -- typically the user just opened the app, often straight
    // from a notification -- so re-notifying for them is redundant. Live messages
    // that arrive after the sync completes still notify normally.
    property var syncingServers: (new Set())
    function isSyncingBacklog() {
        return syncingServers && syncingServers.size > 0
    }

    Connections {
        target: MainSignals
        function onSyncStarted(serverId) { mainWindow.syncingServers.add(serverId) }
        function onSyncEnded(serverId) { mainWindow.syncingServers.delete(serverId) }
    }

    FontLoader { // geologica font
        id: geologicaFont
        source: "resources/fonts/geologica.ttf"
    }

    // Bundled color-emoji font so emoji render identically on every platform
    // (Windows/macOS/Linux/Android) instead of each OS's own set. Registered
    // here so the picker can name it directly (emojiFont.name); message/caption
    // text reaches it through Fonts.emoji(...) — QML's `font` value type has no
    // `families`, so the fallback chain is built in C++ (see src/fonthelper.h).
    FontLoader {
        id: emojiFont
        source: "resources/fonts/NotoColorEmoji.ttf"
    }
    // Force Qt to load/rasterize the color-emoji font's glyph cache once, up
    // front, off-screen — so the very first emoji a user ever sees (e.g. the
    // first one inserted into an otherwise-empty composer) doesn't hit a
    // possible first-use render glitch before the font is "warmed up".
    Text {
        visible: false
        width: 1
        height: 1
        font.family: emojiFont.name
        font.pixelSize: 16
        text: "😀"
    }

    function _clamp01(x) { return Math.max(0, Math.min(1, x)) }

    function invertColor(c) {
        return Qt.rgba(1 - c.r, 1 - c.g, 1 - c.b, c.a)
    }

    function adjustContrast(c, factor) {
        function adj(v) { return _clamp01((v - 0.5) * factor + 0.5) }
        return Qt.rgba(adj(c.r), adj(c.g), adj(c.b), c.a)
    }

    // Linear blend between two colors (t=0 → a, t=1 → b), always opaque.
    // Used to lift the floating chat header / composer pills off the
    // background toward the accent so they read as raised surfaces. Every
    // theme has a dark background + light accent, so this always brightens.
    function mixColors(a, b, t) {
        return Qt.rgba(a.r + (b.r - a.r) * t,
                       a.g + (b.g - a.g) * t,
                       a.b + (b.b - a.b) * t,
                       1)
    }

    // Flatten a translucent color onto an opaque backdrop (src-over), returning
    // the opaque color you actually see. The chat bubbles are a very-low-alpha
    // accent tint whose raw RGB is full accent, so deriving a rim "2% brighter
    // than the bubble" requires first resolving what the bubble looks like over
    // the theme background, then lifting that — not lifting the raw fill.
    function compositeOver(src, dst) {
        var a = src.a
        return Qt.rgba(src.r * a + dst.r * (1 - a),
                       src.g * a + dst.g * (1 - a),
                       src.b * a + dst.b * (1 - a),
                       1)
    }

    // Call-log messages are persisted with a stable, language-independent
    // English key (see CallManager) instead of a translated string, so they
    // can be re-translated live here whenever the UI language changes.
    function callStatusText(text) {
        switch (text) {
        case "Missed Call": return qsTr("Missed Call")
        case "Discarded Call": return qsTr("Discarded Call")
        case "Accepted Call": return qsTr("Accepted Call")
        case "Contact is busy": return qsTr("Contact is busy")
        default: return text
        }
    }

    function formatFileSize(bytes) {
        if (bytes <= 0)
            return "0 B"

        const units = ["B", "KB", "MB", "GB", "TB"]
        let value = bytes
        let unitIndex = 0

        while (value >= 1024 && unitIndex < units.length - 1) {
            value /= 1024
            unitIndex += 1
        }

        const precision = unitIndex === 0 ? 0 : (value >= 10 ? 1 : 2)
        return value.toFixed(precision) + " " + units[unitIndex]
    }

    function formatDuration(ms) {
        const totalSeconds = Math.max(0, Math.round(ms / 1000))
        const minutes = Math.floor(totalSeconds / 60)
        const seconds = totalSeconds % 60
        return minutes + ":" + (seconds < 10 ? "0" + seconds : seconds)
    }

    // Keep only the characters that are legal in an IP/domain:port address.
    // This strips spaces, tabs, \n, \r AND any invisible/zero-width junk
    // (U+200B, U+00AD, nbsp, ...) that the soft keyboard or clipboard injects
    // and that a plain \s replace would miss.
    function sanitizeServerAddress(value) {
        if (!value)
            return ""
        return value.replace(/[^0-9A-Za-z.:\-]/g, "")
    }

    // Debug helper: dump a string as a comma-separated list of char codes so
    // invisible/zero-width characters become visible in logcat.
    function debugCharCodes(value) {
        if (!value)
            return "<empty>"
        let out = []
        for (let i = 0; i < value.length; i++)
            out.push(value.charCodeAt(i))
        return out.join(",")
    }

    // Validate that a sanitized address looks like IP:PORT or DOMAIN:PORT
    // (port mandatory, 1-65535). localhost and bare domains are accepted.
    function isValidServerAddress(value) {
        const v = sanitizeServerAddress(value)
        const lastColon = v.lastIndexOf(":")
        if (lastColon <= 0 || lastColon === v.length - 1)
            return false

        const host = v.substring(0, lastColon)
        const portStr = v.substring(lastColon + 1)

        if (!/^[0-9]{1,5}$/.test(portStr))
            return false
        const port = parseInt(portStr, 10)
        if (port < 1 || port > 65535)
            return false

        const ipMatch = host.match(/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/)
        if (ipMatch) {
            for (let i = 1; i <= 4; i++) {
                if (parseInt(ipMatch[i], 10) > 255)
                    return false
            }
            return true
        }

        const domainRe = /^(?=.{1,253}$)([a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)(\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$/
        return domainRe.test(host)
    }

    function toLocalFileUrl(path) {
        return fileTransferManager.localFileUrl(path)
    }

    // One-line label for a reply quote. `preview` holds concrete text (a text
    // message's body or a file's name); when it is empty the message is a bare
    // media/voice item, so fall back to a localized label chosen by `kind`.
    function replyLabel(kind, preview) {
        if (preview && preview.length > 0)
            return preview
        switch (kind) {
        case "image": return qsTr("Photo")
        case "video": return qsTr("Video")
        case "audio": return qsTr("Voice message")
        case "file":  return qsTr("File")
        case "album": return qsTr("Album")
        default:      return preview
        }
    }

    // Author name shown atop a reply quote. In a 1:1 chat the quoted message is
    // either ours or the current contact's.
    // Pure string logic only: `chats` is a chat-page id and is NOT in this root
    // scope, so callers resolve the contact's names themselves (in a scope where
    // `chats`/`contactsModel` exist) and pass them in. Referencing `chats` here
    // threw a ReferenceError and blanked the quote author.
    function replyAuthorName(isOwn, firstName, lastName) {
        if (isOwn)
            return qsTr("You")
        var full = ((firstName ? firstName : "") + " " + (lastName ? lastName : "")).trim()
        // Mirror the chat header (StyledName), which shows "Anonymous" when a
        // contact has no first/last name — so the quote is never blank.
        return full.length > 0 ? full : qsTr("Anonymous")
    }

    // Builds the reply snapshot fields from a chat message's model roles, used
    // both when starting a reply and when quoting inside a sent bubble.
    function replyPreviewFor(m) {
        if (m.isCall)
            return callStatusText(m.text)  // localized "Incoming/Outgoing/Missed call"
        if (m.isAlbum)
            return m.text ? m.text : ""  // caption, else localized "Album" by kind
        if (m.isAudio || m.isImage || m.isVideo)
            return ""  // bare media: localized at render time by kind
        if (m.isFile)
            return (m.fileName && m.fileName.length > 0) ? m.fileName : ""
        return m.text
    }
    function replyKindFor(m) {
        if (m.isAlbum) return "album"
        if (m.isAudio) return "audio"
        if (m.isImage) return "image"
        if (m.isVideo) return "video"
        if (m.isFile)  return "file"
        return ""
    }

    // Open the fullscreen media viewer on a playlist of items so the user can
    // swipe between them. Each entry: { path, isVideo, title }. Used by albums
    // (all downloaded photos/videos) and, via openMediaViewer, by single media
    // messages (a one-item list).
    function openMediaViewerList(items, startIndex) {
        if (!items || items.length === 0)
            return

        var list = []
        for (var i = 0; i < items.length; ++i) {
            var it = items[i]
            list.push({
                path: it.path,
                url: (it.path && it.path.length > 0) ? toLocalFileUrl(it.path) : "",
                isVideo: it.isVideo === true,
                title: it.title ? it.title : ""
            })
        }

        var idx = startIndex ? startIndex : 0
        if (idx < 0) idx = 0
        if (idx >= list.length) idx = list.length - 1

        if (mediaViewerPopup.opened)
            mediaViewerPopup.close()

        mediaViewerPopup.mediaItems = list
        mediaViewerPopup.startIndex = idx
        mediaViewerPopup.open()
    }

    function openMediaViewer(path, videoMode, title) {
        if (!path || path.length === 0)
            return
        openMediaViewerList([{ path: path, isVideo: videoMode, title: title || "" }], 0)
    }

    function saveFileToMobileDownloads(localPath, displayName) {
        if (!localPath || localPath.length === 0)
            return false

        const safeName = displayName && displayName.length > 0 ? displayName : qsTr("File")
        if (Platform.isAndroid && AndroidSystemUi.available) {
            const savedPath = AndroidSystemUi.saveFileToDownloads(localPath, safeName)
            if (savedPath && savedPath.length > 0) {
                AppNotifier.showNotification(
                    qsTr("Saved to Downloads"),
                    qsTr("%1 saved to Downloads").arg(safeName)
                )
                return true
            }
        }

        AppNotifier.showNotification(qsTr("Save failed"), qsTr("Failed to save file to Downloads"))
        return false
    }

    // Platform-aware save into the Downloads folder: Android via MediaStore,
    // desktop via a direct copy in FileTransferManager.
    function saveToDownloads(localPath, name) {
        if (!localPath || localPath.length === 0)
            return false
        if (Platform.isAndroid && AndroidSystemUi.available)
            return saveFileToMobileDownloads(localPath, name)
        var ok = fileTransferManager.copyToDownloads(localPath, name ? name : "")
        AppNotifier.showNotification(
            ok ? qsTr("Saved to Downloads") : qsTr("Save failed"),
            ok ? qsTr("%1 saved to Downloads").arg(name ? name : qsTr("File"))
               : qsTr("Failed to save file to Downloads"))
        return ok
    }

    function colorLuminance(color) {
        return (0.2126 * color.r) + (0.7152 * color.g) + (0.0722 * color.b)
    }

    function updateSystemBarStyle() {
        if (!Platform.isAndroid || !AndroidSystemUi.available || !theme)
            return

        const darkIcons = colorLuminance(theme.background) > 0.6
        AndroidSystemUi.setStatusBarColor(theme.background, darkIcons)
    }

    function currentMainPage() {
        const currentItem = stack.currentItem
        if (currentItem && currentItem.objectName === "mainPage")
            return currentItem

        return null
    }

    // Bottom-nav-bar routing. "chats" pops back to the chats list; the other
    // tabs push their page (or replace the current tab page, so tab-to-tab
    // switching never stacks Settings→QR→AddContact on top of each other).
    function openChatsTab(key) {
        // Tapping the tab you are already on navigates nowhere, so never prompt
        // to save in that case.
        const cur = stack.currentItem ? stack.currentItem.objectName : ""
        const sameTab = (key === "settings" && cur === "settingsPage")
                     || (key === "add" && cur === "addContactPage")
                     || (key === "qr" && cur === "qrCodePage")
        if (sameTab)
            return
        // Leaving a page with unsaved edits: confirm first, then run the switch.
        if (mainWindow.maybeGuardLeave(function() { mainWindow.performOpenChatsTab(key) }))
            return
        mainWindow.performOpenChatsTab(key)
    }

    function performOpenChatsTab(key) {
        if (key === "chats") {
            while (stack.depth > 1 && stack.currentItem.objectName !== "mainPage")
                stack.pop()
            const mp = currentMainPage()
            if (mp)
                mp.resetMobileNavigation(mp.mobilePaneChats)
            return
        }

        const targetObj = key === "settings" ? "settingsPage"
                        : key === "add" ? "addContactPage"
                        : key === "qr" ? "qrCodePage" : ""
        if (targetObj === "")
            return
        const cur = stack.currentItem ? stack.currentItem.objectName : ""
        if (cur === targetObj)
            return
        const target = key === "settings" ? settingsPage
                     : key === "add" ? addContactPage : qrCodePage
        if (cur === "settingsPage" || cur === "addContactPage" || cur === "qrCodePage")
            stack.replace(target)
        else
            stack.push(target)
    }

    function clearPendingExitBack() {
        if (!exitBackPending)
            return

        exitBackPending = false
        exitBackTimer.stop()
    }

    function shouldConfirmExitFromRoot() {
        const mainPageItem = currentMainPage()
        return Platform.isMobile
            && stack.depth === 1
            && mainPageItem
            && mainPageItem.isMobileLayout
            && !mainPageItem.canNavigateMobileBack()
    }

    function handleRootExitBack() {
        if (!shouldConfirmExitFromRoot()) {
            clearPendingExitBack()
            return false
        }

        if (exitBackPending) {
            clearPendingExitBack()
            return false
        }

        exitBackPending = true
        exitBackTimer.restart()
        AppNotifier.showInfo(qsTr("Press back again to close the app"))
        return true
    }

    function canNavigateBack() {
        if (mediaViewerPopup.opened)
            return true

        const mainPageItem = currentMainPage()
        if (mainPageItem && mainPageItem.isMobileLayout && mainPageItem.canNavigateMobileBack())
            return true

        return stack.depth > 1
    }

    // Unsaved-changes guard. If the page we are about to leave has pending edits
    // it wants confirmed first (the Settings page), hand it the navigation to
    // run once the user decides. Returns true when the page took over (a dialog
    // is now shown), false when the caller should just proceed as usual.
    function maybeGuardLeave(proceed) {
        const it = stack.currentItem
        if (it && it.hasUnsavedChanges === true
                && typeof it.requestLeaveConfirmation === "function") {
            it.requestLeaveConfirmation(proceed)
            return true
        }
        return false
    }

    function navigateBack() {
        if (mediaViewerPopup.opened) {
            clearPendingExitBack()
            mediaViewerPopup.close()
            return true
        }

        // A guarded page already showing its unsaved-changes dialog: back cancels
        // the dialog and keeps us on the page rather than leaving it.
        const currentPage = stack.currentItem
        if (currentPage && typeof currentPage.dismissLeaveConfirmation === "function"
                && currentPage.dismissLeaveConfirmation()) {
            clearPendingExitBack()
            return true
        }

        const mainPageItem = currentMainPage()
        if (mainPageItem && mainPageItem.closeEmojiPickerIfOpen()) {
            clearPendingExitBack()
            return true
        }

        if (mainPageItem && mainPageItem.isMobileLayout && mainPageItem.navigateMobileBack()) {
            clearPendingExitBack()
            return true
        }

        if (stack.depth > 1) {
            if (mainWindow.maybeGuardLeave(function() { clearPendingExitBack(); stack.pop() }))
                return true
            clearPendingExitBack()
            stack.pop()
            return true
        }

        return handleRootExitBack()
    }

    // Theme ===========================================================

    ThemeDark {
        id: darkTheme
    }
    ThemeHighContrast {
        id: highContrastTheme
    }

    ThemeAmber {
        id: amberTheme
    }

    ThemeViolet {
        id: violetTheme
    }

    ThemeOcean {
        id: oceanTheme
    }

    ThemeRose {
        id: roseTheme
    }

    ThemeNord {
        id: nordTheme
    }

    ThemeNight {
        id: nightTheme
    }

    ThemeGraphite {
        id: graphiteTheme
    }

    ThemeIndigo {
        id: indigoTheme
    }

    ThemeCoral {
        id: coralTheme
    }

    ThemeSage {
        id: sageTheme
    }

    ThemeCopper {
        id: copperTheme
    }

    ThemeTelegraph {
        id: telegraphTheme
    }

    ThemeCatalog {
        id: themeCatalog
    }
    property string currentThemeKey: Settings.getTextSetting("themeKey", themeCatalog.defaultTheme)

    property var theme: themeLoader.item
    // Elevated surface for the floating chat header / composer / nav pills: the
    // theme background lifted ~3% toward white (a subtle, low-contrast neutral
    // lightening, like an #FFFFFF overlay at 3%) rather than tinted to accent.
    readonly property color chatPanelSurface: theme ? mainWindow.mixColors(theme.background, Qt.rgba(1, 1, 1, 1), 0.02) : "transparent"
    // Hairline highlight for those same panels' top/bottom edges (see
    // PanelBorder.qml) — the faint accent tint (aroundZeroAccent) so the rim
    // picks up the theme colour instead of a neutral white overlay.
    readonly property color chatPanelBorderColor: theme ? theme.aroundZeroAccent : "transparent"
    property bool exitBackPending: false
    property bool backGestureHandled: false
    property bool liveMediaPreviewEnabled: Settings.getBoolSetting("liveMediaPreview", true)

    function setTheme(themeKey) {
        currentThemeKey = themeKey
        Settings.setTextSetting("themeKey", themeKey)
        themeLoader.source = themeCatalog.sourceFor(themeKey) // sourceFor already handles fallback
    }

    onThemeChanged: updateSystemBarStyle()

    Loader {
        id: themeLoader
        asynchronous: false
        active: true
        source: themeCatalog.sourceFor(mainWindow.currentThemeKey)
    }

    // =================================================================


    Timer {
        id: exitBackTimer
        interval: 2000
        repeat: false
        onTriggered: mainWindow.exitBackPending = false
    }
    Timer {
        id: backGestureHandledTimer
        interval: 400
        repeat: false
        onTriggered: mainWindow.backGestureHandled = false
    }
    // Account reset / delete. Optionally tells every contact to clear the
    // conversation on their side (same e2e path as the per-chat "Clear history"),
    // then wipes the local database — contacts, messages, identity keys and all
    // settings — and returns to onboarding.
    function performAccountReset(clearRemote) {
        if (clearRemote) {
            var contacts = contactsModel.getContacts()
            for (var key in contacts) {
                var c = contacts[key]
                if (c && c.server && c.server.length > 0
                    && c.pubkeyFingerprint && c.pubkeyFingerprint.length > 0)
                    MainSignals.emitClearHistory(c.server, c.pubkeyFingerprint)
            }
            // Let the e2e clear messages flush before tearing everything down.
            accountResetTimer.restart()
        } else {
            mainWindow.finalizeAccountReset()
        }
    }
    function finalizeAccountReset() {
        connections.removeAllServers()
        db.wipeAllData()
        Settings.clearCache()
        Account.reloadIdentityCache()
        messageModel.closeCurrentChat()
        contactsModel.loadFromDatabase()
        Qt.callLater(function() {
            stack.clear()
            stack.push(welcomePage)
        })
    }
    Timer {
        id: accountResetTimer
        interval: 1500
        repeat: false
        onTriggered: mainWindow.finalizeAccountReset()
    }
    Connections {
        target: voiceMessageManager
        function onErrorOccurred(errorText) {
            if (errorText && errorText.length > 0)
                AppNotifier.showNotification(qsTr("Error"), errorText)
        }
    }
    Connections {
        target: callManager
        function onAudioError(error) {
            if (error && error.length > 0)
                AppNotifier.showNotification(qsTr("Call error"), error)
        }
    }
    Connections {
        target: connections
        function onProtocolVersionMismatch(serverId, message) {
            if (!message || message.length === 0)
                return

            console.warn("Protocol version mismatch for", serverId, message)
            const serverText = serverId && serverId.length > 0
                ? qsTr("Server: %1").arg(serverId)
                : qsTr("Server: unknown")
            AppNotifier.showNotification(
                qsTr("Incompatible protocol version"),
                serverText + "\n" + message
            )
        }
    }
    Connections {
        target: MainSignals
        function onSettingSetBool(key, value) {
            if (key === "liveMediaPreview")
                mainWindow.liveMediaPreviewEnabled = value
        }
        // A contact's identity key was reported compromised — either in-band from
        // them (signature already verified) or relayed by a server when we tried
        // to reach them. New sessions with the key are refused server-side; here we
        // just make sure the user sees it. Existing sessions must be treated as
        // untrusted until the contact is re-verified in person.
        function onIdentityRevocationReceived(serverId, contactPubKey) {
            // The contact now shows a persistent HACKED badge (ContactsModel handles
            // the flag); here we just raise a one-off notification so the user notices.
            console.warn("Identity revocation received for", contactPubKey, "on", serverId)
            AppNotifier.showNotification(
                qsTr("Contact key compromised"),
                qsTr("A contact reported their key as hacked. Do not trust it until you re-verify in person."))
        }
    }
    Connections {
        target: UpdateManager
        function onErrorOccurred(message) {
            if (message && message.length > 0)
                AppNotifier.showNotification(qsTr("Update error"), message)
        }
    }
    MediaPlayer { // notification sound
        id: notificationSound
        source: "resources/sounds/notification.wav"
        audioOutput: AudioOutput {}
    }
    MediaPlayer { // notification sound for current chat
        id: notificationCurrentChatSound
        source: "resources/sounds/notification_current_chat.wav"
        audioOutput: AudioOutput {}
    }
    Popup {
        id: mediaViewerPopup
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        width: mainWindow.width
        height: mainWindow.height
        x: 0
        y: 0
        padding: 0

        // Playlist shown in the viewer. Each entry:
        //   { path: <local path>, url: <file url>, isVideo: bool, title: string }
        // A single media message opens a one-item list; an album opens with all
        // of its downloaded photos/videos so they can be swiped between.
        property var mediaItems: []
        property int startIndex: 0

        readonly property var currentPage: mediaViewerSwipe.currentItem
        readonly property string currentTitle: (currentPage && currentPage.mediaTitle) ? currentPage.mediaTitle : ""

        onOpened: mediaViewerSwipe.setCurrentIndex(startIndex)

        onClosed: {
            mediaItems = []
            startIndex = 0
        }

        Overlay.modal: Rectangle {
            color: Qt.rgba(0, 0, 0, 0.92)
        }

        background: Item {}

        Item {
            anchors.fill: parent

            SwipeView {
                id: mediaViewerSwipe
                anchors.fill: parent
                clip: true
                // While the current photo is zoomed in, a drag pans the image
                // instead of paging, so disable swiping to keep the two gestures
                // from fighting. Swiping is also pointless with a single item.
                interactive: count > 1 && !(currentItem && currentItem.zoomed)

                function setCurrentIndex(i) {
                    if (count <= 0)
                        return
                    if (i < 0) i = 0
                    if (i >= count) i = count - 1
                    currentIndex = i
                }

                Repeater {
                    model: mediaViewerPopup.mediaItems
                    delegate: mediaViewerPageComponent
                }
            }

            // ---- Top bar: title + counter + close ----
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 20
                height: 44
                radius: 16
                color: "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 6
                    spacing: 12

                    Text {
                        Layout.fillWidth: true
                        text: mediaViewerPopup.currentTitle.length > 0 ? mediaViewerPopup.currentTitle : qsTr("Media")
                        color: mainWindow.theme.accent
                        font.pixelSize: 10
                        font.family: geologicaFont.name
                        font.weight: Font.DemiBold
                        elide: Text.ElideMiddle
                    }

                    Text {
                        visible: mediaViewerSwipe.count > 1
                        text: (mediaViewerSwipe.currentIndex + 1) + " / " + mediaViewerSwipe.count
                        color: mainWindow.theme.halfAccent
                        font.pixelSize: 10
                        font.family: geologicaFont.name
                        font.weight: Font.DemiBold
                    }

                    BottomButton {
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        icon.source: "resources/close.svg"
                        icon.color: mainWindow.theme.accent
                        icon.width: 14
                        icon.height: 14
                        onClicked: mediaViewerPopup.close()
                    }
                }
            }
        }

        // ---- One swipe page: a zoomable photo or a playable video ----
        Component {
            id: mediaViewerPageComponent

            Item {
                id: page

                readonly property bool isVideo: modelData.isVideo === true
                readonly property string mediaTitle: modelData.title ? modelData.title : ""
                readonly property bool isCurrentPage: SwipeView.isCurrentItem
                // Read by SwipeView.interactive to block paging while zoomed in.
                property bool zoomed: imageLoader.item ? imageLoader.item.zoomed : false

                onIsCurrentPageChanged: {
                    if (!isCurrentPage && imageLoader.item)
                        imageLoader.item.resetZoom()
                }

                // -------- Photo (pinch / wheel / double-tap zoom + drag pan) --------
                Loader {
                    id: imageLoader
                    anchors.fill: parent
                    active: !page.isVideo
                    visible: active
                    sourceComponent: Item {
                        id: zoomer
                        anchors.fill: parent
                        clip: true

                        readonly property real minScale: 1.0
                        readonly property real maxScale: 5.0
                        property bool zoomed: photo.scale > 1.01

                        function resetZoom() {
                            photo.scale = 1.0
                            photo.x = 0
                            photo.y = 0
                        }

                        // Zoom to targetScale, keeping the point (cx, cy) stationary
                        // (center-origin scaling => translate by (p - center)(1 - s)).
                        function zoomTo(targetScale, cx, cy) {
                            targetScale = Math.max(minScale, Math.min(maxScale, targetScale))
                            if (targetScale <= 1.001) {
                                resetZoom()
                                return
                            }
                            photo.x = (cx - zoomer.width / 2) * (1 - targetScale)
                            photo.y = (cy - zoomer.height / 2) * (1 - targetScale)
                            photo.scale = targetScale
                        }

                        Image {
                            id: photo
                            width: zoomer.width
                            height: zoomer.height
                            transformOrigin: Item.Center
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: false
                            smooth: true
                            mipmap: true
                            source: modelData.url ? modelData.url : ""

                            Behavior on scale {
                                enabled: !pinch.active
                                NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                            }
                            Behavior on x {
                                enabled: !pinch.active && !drag.active
                                NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                            }
                            Behavior on y {
                                enabled: !pinch.active && !drag.active
                                NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                            }
                        }

                        PinchHandler {
                            id: pinch
                            target: photo
                            minimumScale: zoomer.minScale
                            maximumScale: zoomer.maxScale
                            // Lock rotation — the photo must never tilt while pinching.
                            minimumRotation: 0
                            maximumRotation: 0
                            onActiveChanged: if (!active && photo.scale <= 1.001)
                                zoomer.resetZoom()
                        }

                        DragHandler {
                            id: drag
                            target: photo
                            // Pan freely, including past the image edges — no snap-back.
                            enabled: zoomer.zoomed
                        }

                        WheelHandler {
                            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                            onWheel: function(event) {
                                var factor = event.angleDelta.y > 0 ? 1.2 : (1 / 1.2)
                                zoomer.zoomTo(photo.scale * factor, event.x, event.y)
                            }
                        }

                        TapHandler {
                            acceptedButtons: Qt.LeftButton
                            onDoubleTapped: function(eventPoint) {
                                if (zoomer.zoomed)
                                    zoomer.zoomTo(1.0, zoomer.width / 2, zoomer.height / 2)
                                else
                                    zoomer.zoomTo(2.5, eventPoint.position.x, eventPoint.position.y)
                            }
                        }
                    }
                }

                // -------- Video (own player, plays only while current) --------
                Loader {
                    id: videoLoader
                    anchors.fill: parent
                    active: page.isVideo
                    visible: active
                    sourceComponent: Item {
                        anchors.fill: parent

                        AudioOutput { id: pageAudio; volume: 1.0 }
                        MediaPlayer {
                            id: pagePlayer
                            source: modelData.url ? modelData.url : ""
                            audioOutput: pageAudio
                            videoOutput: pageVideo
                        }

                        Connections {
                            target: page
                            function onIsCurrentPageChanged() {
                                if (page.isCurrentPage) {
                                    pagePlayer.play()
                                } else {
                                    pagePlayer.pause()
                                    pagePlayer.position = 0
                                }
                            }
                        }
                        Component.onCompleted: if (page.isCurrentPage) pagePlayer.play()
                        Component.onDestruction: pagePlayer.stop()

                        VideoOutput {
                            id: pageVideo
                            anchors.fill: parent
                            anchors.topMargin: 72
                            anchors.bottomMargin: 92
                            fillMode: VideoOutput.PreserveAspectFit
                        }

                        // TapHandler (not MouseArea) so a horizontal drag still
                        // reaches the SwipeView and pages between videos — a
                        // full-screen MouseArea would swallow the swipe.
                        TapHandler {
                            acceptedButtons: Qt.LeftButton
                            onTapped: {
                                if (pagePlayer.playbackState === MediaPlayer.PlayingState)
                                    pagePlayer.pause()
                                else
                                    pagePlayer.play()
                            }
                        }

                        Rectangle {
                            anchors.centerIn: parent
                            visible: pagePlayer.playbackState !== MediaPlayer.PlayingState
                            width: 64
                            height: 64
                            radius: 32
                            color: Qt.rgba(0, 0, 0, 0.45)
                            ColorImage {
                                anchors.centerIn: parent
                                width: 30
                                height: 30
                                source: "resources/play.svg"
                                accentColor: "white"
                            }
                        }

                        // Bottom control bar
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 20
                            height: 56
                            radius: 18
                            color: Qt.rgba(0, 0, 0, 0.35)

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 14
                                spacing: 12

                                BottomButton {
                                    Layout.preferredWidth: 36
                                    Layout.preferredHeight: 36
                                    icon.source: pagePlayer.playbackState === MediaPlayer.PlayingState ? "resources/pause.svg" : "resources/play.svg"
                                    icon.color: mainWindow.theme.accent
                                    icon.width: 18
                                    icon.height: 18
                                    onClicked: {
                                        if (pagePlayer.playbackState === MediaPlayer.PlayingState)
                                            pagePlayer.pause()
                                        else
                                            pagePlayer.play()
                                    }
                                }

                                Slider {
                                    Layout.fillWidth: true
                                    from: 0
                                    to: Math.max(pagePlayer.duration, 1)
                                    value: pagePlayer.position
                                    onMoved: pagePlayer.setPosition(value)
                                }

                                Text {
                                    text: mainWindow.formatDuration(pagePlayer.position) + " / " + mainWindow.formatDuration(pagePlayer.duration)
                                    color: mainWindow.theme.halfAccent
                                    font.pixelSize: 11
                                    font.family: geologicaFont.name
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    Material.theme: Material.Dark
    Material.accent: theme.accent
    flags: Platform.useCustomWindowFrame ? Qt.FramelessWindowHint : Qt.Window
    property bool isWindowMaximized: false

    // Top safe-area inset (status bar / notch). Read from a fixed full-window
    // probe (safeAreaProbe) so it reflects the raw window inset, not a value that
    // shrinks once we consume it. Non-zero only when the platform draws our
    // content edge-to-edge under the system bars (Android 15+). On older Android
    // the window is already inset below the status bar, so this is 0 and we add
    // no extra top gap.
    readonly property real safeTopInset: Platform.isMobile ? safeAreaProbe.SafeArea.margins.top : 0
    // Bottom safe-area inset (navigation bar). Mirrors safeTopInset, but gated on
    // edge-to-edge so it is non-zero ONLY when we draw beneath the system bars
    // (Android 15+/SDK>=35). There the window extends under the navigation bar, so
    // bottom-anchored content falls under it — invisible-ish behind the thin
    // gesture pill, but hidden behind the tall opaque 3-button nav bar. Reserve
    // that space. On older Android the system already insets the window below the
    // nav bar, so this stays 0 and nothing shifts (no regression); the explicit
    // edgeToEdge gate also stops a stray non-zero SafeArea report on a pre-inset
    // window from ever double-insetting. Trimmed a little: bottom-anchored content
    // already carries its own cosmetic margin (the floating nav bar, input bar,
    // etc.), so reserving the full nav-bar height on top read as too much air —
    // shave a bit off (clamped at 0) so content sits closer to the bar without
    // going under it.
    readonly property real bottomInsetTrim: 18
    readonly property real safeBottomInset: (Platform.isMobile && AndroidSystemUi.available && AndroidSystemUi.edgeToEdge) ? Math.max(0, safeAreaProbe.SafeArea.margins.bottom - bottomInsetTrim) : 0
    property real restoreX: 0
    property real restoreY: 0
    property real restoreWidth: 1024
    property real restoreHeight: 640

    function centerOnCurrentScreen() {
        if (!screen) {
            return
        }

        x = screen.virtualX + (screen.width - width) / 2
        y = screen.virtualY + (screen.height - height) / 2
    }

    function toggleMaximize() {
        if (isWindowMaximized) {
            mainWindow.showNormal()
            x = restoreX
            y = restoreY
            width = restoreWidth
            height = restoreHeight
            isWindowMaximized = false
            return
        }

        restoreX = x
        restoreY = y
        restoreWidth = width
        restoreHeight = height

        mainWindow.showMaximized()
        isWindowMaximized = true
    }
    width: 1024
    height: 640
    minimumHeight: 480
    minimumWidth: 640
    color: theme.background
    visible: true
    title: qsTr("Patronus")
    Component.onCompleted: {
        if (Platform.useFullScreenWindow) {
            mainWindow.showFullScreen()
        } else if (Platform.useMaximizedWindow) {
            mainWindow.showMaximized()
        } else {
            centerOnCurrentScreen()
        }

        Qt.callLater(updateSystemBarStyle)
    }
    onVisibilityChanged: {
        isWindowMaximized = visibility === Window.Maximized || visibility === Window.FullScreen
    }
    Keys.onReleased: function(event) {
        if (event.key === Qt.Key_Back || event.key === Qt.Key_Escape) {
            if (mainWindow.navigateBack()) {
                mainWindow.backGestureHandled = true
                backGestureHandledTimer.restart()
                event.accepted = true
            }
        }
    }
    onClosing: function(close) {
        if (Platform.isMobile && !mainWindow.backGestureHandled && mainWindow.navigateBack())
            close.accepted = false
    }
    MouseArea { // left edge
        visible: Platform.showResizeHandles
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Platform.showResizeHandles ? 5 : 0
        enabled: Platform.showResizeHandles
        cursorShape: Qt.SizeHorCursor
        onPressed: mainWindow.startSystemResize(Qt.LeftEdge)
        z: 500
    }
    MouseArea { // right edge
        visible: Platform.showResizeHandles
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Platform.showResizeHandles ? 5 : 0
        enabled: Platform.showResizeHandles
        cursorShape: Qt.SizeHorCursor
        onPressed: mainWindow.startSystemResize(Qt.RightEdge)
        z: 500
    }
    MouseArea { // top edge
        visible: Platform.showResizeHandles
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Platform.showResizeHandles ? 5 : 0
        enabled: Platform.showResizeHandles
        cursorShape: Qt.SizeVerCursor
        onPressed: mainWindow.startSystemResize(Qt.TopEdge)
        z: 500
    }
    MouseArea { // bottom edge
        visible: Platform.showResizeHandles
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: Platform.showResizeHandles ? 5 : 0
        enabled: Platform.showResizeHandles
        cursorShape: Qt.SizeVerCursor
        onPressed: mainWindow.startSystemResize(Qt.BottomEdge)
        z: 500
    }
    Item { // safe-area probe: fixed, full-window; exposes system insets via SafeArea
        id: safeAreaProbe
        anchors.fill: parent
        z: -1
    }
    Rectangle { // top panel
        id: topPanel
        width: parent.width
        height: 25
        z: 300
        color: theme.background
        visible: Platform.useCustomWindowFrame
        RowLayout { 
            id: topPanel2
            width: parent.width
            height: 25
            visible: Platform.useCustomWindowFrame
            Rectangle {
                Layout.fillHeight: true
                color: "transparent"
                Logo {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 15
                    height: 15
                    accentColor: theme.accent
                    MouseArea {
                        anchors.fill: parent
                        enabled: Platform.useCustomWindowFrame
                        onPressed: {
                            mainWindow.startSystemMove()
                        }
                    }
                }
                Text { // title
                    text: "Patronus"
                    color: theme.accent
                    font.pixelSize: 11
                    font.family: "Inter"
                    font.weight: Font.Bold
                    anchors.left: parent.left
                    anchors.leftMargin: 30
                    anchors.verticalCenter: parent.verticalCenter
                    MouseArea {
                        anchors.fill: parent
                        enabled: Platform.useCustomWindowFrame
                        onPressed: {
                            mainWindow.startSystemMove()
                        }
                    }
                }
            }
            MouseArea { // drag window
                Layout.fillWidth: true
                Layout.fillHeight: true
                enabled: Platform.useCustomWindowFrame
                onPressed: {
                    mainWindow.startSystemMove()
                }
            }
            RowLayout { // window control buttons
                visible: Platform.showWindowControls
                Layout.alignment: Qt.AlignRight
                Layout.fillHeight: true
                Layout.fillWidth: true
                Layout.margins: 0
                Layout.bottomMargin: 0
                Layout.topMargin: 0
                Layout.rightMargin: 0
                Layout.leftMargin: 0
                spacing: 0
                PanelButton {
                    Layout.alignment: Qt.AlignRight
                    icon.source: "resources/minimize.svg"
                    icon.width: 4
                    icon.height: 1
                    icon.color: theme.accent
                    onClicked: {
                        mainWindow.showMinimized()
                    }
                }
                PanelButton { // maximize/restore button
                    Layout.alignment: Qt.AlignRight
                    enabled: stack.currentItem && stack.currentItem.objectName !== "welcomePage" && stack.currentItem.objectName !== "regPage" && stack.currentItem.objectName !== "warnPage" && stack.currentItem.objectName !== "createPage" && stack.currentItem.objectName !== "profileSetupPage"
                    icon.source: mainWindow.isWindowMaximized ? "resources/restore.svg" : "resources/maximize.svg"
                    icon.color: enabled ? theme.accent : theme.lowAccent
                    onClicked: {
                        mainWindow.toggleMaximize()
                    }
                }
                PanelButton { // close button
                    id: closeButton
                    Layout.alignment: Qt.AlignRight
                    icon.source: "resources/close.svg"
                    icon.color: theme.accent
                    onClicked: {
                        mainWindow.close()
                    }
                    HoverHandler {
                        onHoveredChanged: {
                            closeButton.panelBackground = hovered ? theme.redAccent : "transparent"
                            closeButton.icon.color = hovered ? theme.background : theme.accent
                        }
                    }
                }
            }
        }
    } // top panel
    StackView { // main stack of pages
        id: stack
        // Mobile: sit directly at the window top and drop by the real status-bar
        // inset (0 on non-edge-to-edge Android, where the window is already
        // inset). Desktop: below the custom title bar. This replaces the old
        // fixed ~45px gap that double-inset the content on Android 11.
        anchors.top: Platform.isMobile ? parent.top : topPanel.bottom
        // On edge-to-edge (Android 15+) the status bar is a transparent overlay,
        // so add breathing room below it — otherwise the header hugs the bar.
        // Non-edge-to-edge Android already has a solid status-bar shelf, so it
        // needs no extra gap (and adding one would reintroduce a visible offset).
        anchors.topMargin: Platform.isMobile ? mainWindow.safeTopInset + (AndroidSystemUi.available && AndroidSystemUi.edgeToEdge ? 6 : 0) : 0
        anchors.bottom: bottomPanel.top
        // Reserve the navigation-bar inset (edge-to-edge only) so content never
        // falls under the 3-button nav bar, plus the keyboard height when the IME
        // is up. keyboardHeight already excludes the nav bar (ime.bottom -
        // nav.bottom), so safeBottomInset + keyboardHeight == the full IME top.
        // On non-edge-to-edge Android both terms are 0: the window is pre-inset
        // below the nav bar and Qt's ADJUST_RESIZE shrinks it for the keyboard.
        // emojiPanelHeight reserves the SAME space for the docked mobile emoji
        // picker, which replaces the keyboard rather than stacking on top of
        // it — Math.max (not +) keeps the reserved space from briefly doubling
        // up while the real keyboard is still animating closed as the panel
        // opens (see emojiButton).
        anchors.bottomMargin: mainWindow.safeBottomInset + Math.max(
            (Platform.isAndroid && AndroidSystemUi.available && AndroidSystemUi.edgeToEdge) ? AndroidSystemUi.keyboardHeight : 0,
            mainWindow.emojiPanelHeight)
        Behavior on anchors.bottomMargin {
            enabled: Platform.isAndroid && AndroidSystemUi.available && !mainWindow.suppressBottomMarginAnimation
            // onProgress fires at 60fps but QueuedConnection can batch updates,
            // causing discrete jumps. SmoothedAnimation at high velocity
            // interpolates between batched steps without lagging behind keyboard.
            SmoothedAnimation { velocity: 8000; easing.type: Easing.OutCubic }
        }
        anchors.left: parent.left
        anchors.right: parent.right
        width: parent.width
        initialItem: (!AppSession.databaseOpen) ? passwordPage :
                     (!public_key_found) ? welcomePage : mainPage
    }
    Item {
        anchors.top: stack.top
        anchors.bottom: stack.bottom
        anchors.left: stack.left
        anchors.right: stack.right
        visible: Platform.isMobile
        z: 1000

        MouseArea {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 18
            enabled: mainWindow.canNavigateBack()
            preventStealing: true
            property real startX: 0

            onPressed: function(mouse) {
                startX = mouse.x
            }

            onReleased: function(mouse) {
                if (mouse.x - startX >= 48) {
                    mainWindow.backGestureHandled = true
                    backGestureHandledTimer.restart()
                    mainWindow.navigateBack()
                }
            }
        }

        // Persistent bottom tab bar (mobile). Lives in the root StackView overlay
        // so it stays put across Chats · Settings · Add contact · My QR instead of
        // being covered when those pages get pushed. The active tab tracks the
        // current stack page; taps route through mainWindow.openChatsTab().
        Rectangle {
            id: bottomNavBar
            readonly property string activeKey: {
                var it = stack.currentItem
                if (!it)
                    return ""
                switch (it.objectName) {
                case "settingsPage": return "settings"
                case "addContactPage": return "add"
                case "qrCodePage": return "qr"
                case "mainPage": return it.atChatsList ? "chats" : ""
                }
                return ""
            }
            visible: activeKey !== ""
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            // 18, not 22, so this pill's visual bottom lines up with the input
            // bar's: the composer sits at 22 but its inputBarBg overhangs 4px
            // lower (-4), landing at 18 — match that here.
            // parent (the overlay Item) shrinks along with stack's bottom
            // margin when the IME opens, which would otherwise drag this
            // pill up above the keyboard. Cancel out that keyboard-only
            // portion so the pill stays pinned in place (covered by the
            // keyboard) instead of floating over it.
            anchors.bottomMargin: 18 - (stack.anchors.bottomMargin - mainWindow.safeBottomInset)
            height: 58
            radius: height / 2
            color: mainWindow.chatPanelSurface

            PanelBorder {
                edgeColor: mainWindow.chatPanelBorderColor
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 0

                Repeater {
                    model: [
                        { key: "chats",    icon: "resources/chats.svg" },
                        { key: "qr",       icon: "resources/qr.svg" },
                        { key: "add",      icon: "resources/add_contact.svg" },
                        { key: "settings", icon: "resources/settings.svg" }
                    ]
                    delegate: Item {
                        id: navTab
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        readonly property bool active: modelData.key === bottomNavBar.activeKey

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.topMargin: 9
                            anchors.bottomMargin: 9
                            spacing: 0

                            ColorImage {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 16
                                source: modelData.icon
                                // Active state is conveyed purely by color, no background chip.
                                accentColor: navTab.active ? theme.accent : theme.lowAccent
                            }
                            Text {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                                text: modelData.key === "chats" ? qsTr("Chats")
                                    : modelData.key === "settings" ? qsTr("Settings")
                                    : modelData.key === "add" ? qsTr("Add contact")
                                    : qsTr("My QR")
                                color: navTab.active ? theme.accent : theme.lowAccent
                                font.family: geologicaFont.name
                                font.pixelSize: 9
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: mainWindow.openChatsTab(modelData.key)
                        }
                    }
                }
            }
        }

    }
    Component { // troubleshooting page — live connection-health chain + copyable log
        id: troubleshootingPage
        Page {
            objectName: "troubleshootingPage"
            Rectangle {
                id: tsRoot
                anchors.fill: parent
                color: theme.background

                readonly property int rowH: Platform.isMobile ? 60 : 46
                readonly property int dotSize: 16
                readonly property int dotLeft: 8

                property string serverAddr: mainWindow.sanitizeServerAddress(Settings.getTextSetting("serverAddress", ""))
                property bool showLog: false
                // Bumped by the connection signals so appState re-evaluates.
                property int connVersion: 0
                // WS heartbeat: outcome + round-trip of the last ping. The transport
                // pings every ~10s of silence and drops the link if no pong in 20s,
                // so a live "Connected" node == the previous ping passed.
                property int pingState: 0
                property real lastPongMs: -1

                // 0 Idle · 1 InProgress · 2 Ok · 3 Failed (mirrors DiagnosticsManager::StepState)
                function stepColor(state) {
                    return state === 2 ? theme.accent
                         : state === 1 ? theme.yellowAccent
                         : state === 3 ? theme.redAccent
                         : theme.lowAccent
                }

                // Coarse lifecycle from the backend, single source of truth:
                // 0 not connected · 2 authenticating · 3 synchronizing · 4 ready.
                property int phase: {
                    connVersion // establish the dependency
                    if (serverAddr.length === 0)
                        return 0
                    return connections.connectionPhase(serverAddr)
                }
                // "Server application" (authentication handshake) step state.
                property int appState: {
                    if (serverAddr.length === 0)
                        return 3
                    if (phase >= 3) return 2   // authenticated (and beyond)
                    if (phase === 2) return 1  // authenticating…
                    return 3                   // not connected
                }
                // "Synchronization" (offline backlog drain) step state.
                property int syncState: {
                    if (serverAddr.length === 0)
                        return 0
                    if (phase >= 4) return 2   // up to date
                    if (phase === 3) return 1  // synchronizing…
                    return 0                   // not reached yet
                }

                function dnsSub() {
                    switch (diagnostics.dnsState) {
                    case 1: return qsTr("Resolving…")
                    case 2: return diagnostics.resolvedIp
                    case 3: return qsTr("Could not resolve host")
                    default: return "—"
                    }
                }
                function portSub() {
                    switch (diagnostics.portState) {
                    case 1: return qsTr("Connecting…")
                    case 2: return qsTr("Port %1 open · %2 ms").arg(diagnostics.port).arg(diagnostics.portLatencyMs)
                    case 3: return qsTr("Port %1 unreachable").arg(diagnostics.port)
                    default: return "—"
                    }
                }
                function appSub() {
                    switch (appState) {
                    case 1: return qsTr("Authenticating…")
                    case 2: return qsTr("Authenticated")
                    default: return serverAddr.length === 0 ? qsTr("No server configured") : qsTr("Not connected")
                    }
                }
                function syncSub() {
                    switch (syncState) {
                    case 1: return qsTr("Synchronizing...")
                    case 2: return qsTr("Up to date")
                    default: return "—"
                    }
                }
                function pingSub() {
                    connVersion // establish the dependency
                    if (serverAddr.length === 0)
                        return qsTr("No server configured")
                    if (pingState === 2)
                        return lastPongMs >= 0 ? qsTr("Alive · last pong %1 ms").arg(Math.round(lastPongMs)) : qsTr("Alive")
                    if (pingState === 3)
                        return qsTr("No pong — link down")
                    return qsTr("Waiting for heartbeat…")
                }

                property var steps: [
                    { title: qsTr("Your device"),        sub: qsTr("Active"), state: 2 },
                    { title: qsTr("DNS resolve"),        sub: dnsSub(),       state: diagnostics.dnsState },
                    { title: qsTr("Server connection"),  sub: portSub(),      state: diagnostics.portState },
                    { title: qsTr("Server application"), sub: appSub(),       state: appState },
                    { title: qsTr("Synchronization"),    sub: syncSub(),      state: syncState },
                    { title: qsTr("Connected"),          sub: pingSub(),      state: pingState }
                ]

                Connections {
                    target: connections
                    // A pong means the last heartbeat round-tripped: the link is alive.
                    function onPongReceived(serverId, elapsedTime) {
                        if (serverId === tsRoot.serverAddr) {
                            tsRoot.lastPongMs = elapsedTime
                            tsRoot.pingState = 2
                        }
                    }
                    function onAuthChanged(serverId, success) {
                        if (serverId === tsRoot.serverAddr) tsRoot.connVersion++
                    }
                    function onAuthenticated(serverId) {
                        if (serverId === tsRoot.serverAddr) tsRoot.connVersion++
                    }
                    function onOfflineSyncStarted(serverId) {
                        if (serverId === tsRoot.serverAddr) tsRoot.connVersion++
                    }
                    function onOfflineSyncEnded(serverId) {
                        if (serverId === tsRoot.serverAddr) tsRoot.connVersion++
                    }
                    function onConnectionStatusChanged(serverId, status) {
                        if (serverId === tsRoot.serverAddr) {
                            // A dropped link means the last ping failed (timed out).
                            tsRoot.pingState = connections.isConnected(serverId) ? 2 : 3
                            tsRoot.connVersion++
                        }
                    }
                }

                Component.onCompleted: {
                    tsRoot.pingState = tsRoot.serverAddr.length === 0
                        ? 0
                        : (connections.isConnected(tsRoot.serverAddr) ? 2 : 3)
                    diagnostics.runChecks(tsRoot.serverAddr)
                    // Desktop shows the log permanently in its own right-hand pane.
                    if (!Platform.isMobile) {
                        tsRoot.showLog = true
                        logArea.text = diagnostics.logText()
                    }
                }

                BackButton {
                    id: tsBack
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: Platform.isMobile ? 20 : 14
                    anchors.leftMargin: 20
                }
                Text {
                    id: tsTitle
                    text: qsTr("Connection troubleshooting")
                    color: theme.text
                    font.pixelSize: Platform.isMobile ? 18 : 22
                    font.family: geologicaFont.name
                    font.weight: Font.Bold
                    anchors.top: tsBack.bottom
                    anchors.topMargin: Platform.isMobile ? 16 : 10
                    anchors.left: parent.left
                    anchors.leftMargin: 20
                }
                Text {
                    id: tsServer
                    text: tsRoot.serverAddr.length > 0 ? tsRoot.serverAddr : qsTr("No server configured")
                    color: theme.accent
                    font.pixelSize: 13
                    font.family: geologicaFont.name
                    elide: Text.ElideRight
                    width: parent.width - 40
                    anchors.top: tsTitle.bottom
                    anchors.topMargin: 6
                    anchors.left: parent.left
                    anchors.leftMargin: 20
                }

                // The health chain: device → DNS → port → application.
                Item {
                    id: chain
                    anchors.top: tsServer.bottom
                    anchors.topMargin: Platform.isMobile ? 28 : 16
                    // Desktop puts the health chain in a fixed-width left column so
                    // the log panel can take the whole remaining width; mobile keeps
                    // it centered.
                    anchors.horizontalCenter: Platform.isMobile ? parent.horizontalCenter : undefined
                    anchors.left: Platform.isMobile ? undefined : parent.left
                    anchors.leftMargin: Platform.isMobile ? 0 : 24
                    width: Platform.isMobile ? Math.min(parent.width - 40, 520) : 380
                    height: tsRoot.rowH * tsRoot.steps.length

                    Column {
                        anchors.fill: parent
                        Repeater {
                            model: tsRoot.steps
                            delegate: Item {
                                width: chain.width
                                height: tsRoot.rowH

                                Rectangle { // connector down to the next node
                                    visible: index < tsRoot.steps.length - 1
                                    width: 2
                                    height: tsRoot.rowH
                                    color: theme.lowAccent
                                    x: tsRoot.dotLeft + tsRoot.dotSize / 2 - 1
                                    y: tsRoot.rowH / 2
                                    z: -1
                                }
                                Rectangle {
                                    id: nodeDot
                                    width: tsRoot.dotSize
                                    height: tsRoot.dotSize
                                    radius: width / 2
                                    antialiasing: true
                                    // An Ok node gets the same cyan→green gradient as the
                                    // home-server connection dot in the bottom bar; other
                                    // states stay a flat status colour.
                                    color: modelData.state === 2 ? "transparent" : tsRoot.stepColor(modelData.state)
                                    gradient: modelData.state === 2 ? nodeOkGradient : null
                                    anchors.left: parent.left
                                    anchors.leftMargin: tsRoot.dotLeft
                                    anchors.verticalCenter: parent.verticalCenter
                                    Gradient {
                                        id: nodeOkGradient
                                        orientation: Gradient.Horizontal
                                        GradientStop { position: 0.0; color: "#00c4dc" }
                                        GradientStop { position: 1.0; color: "#00ef96" }
                                    }
                                }
                                Column {
                                    anchors.left: nodeDot.right
                                    anchors.leftMargin: 16
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2
                                    Text {
                                        text: modelData.title
                                        color: theme.text
                                        font.pixelSize: 15
                                        font.family: geologicaFont.name
                                        font.weight: Font.Medium
                                    }
                                    Text {
                                        width: parent.width
                                        text: modelData.sub
                                        color: tsRoot.stepColor(modelData.state)
                                        font.pixelSize: 12
                                        font.family: geologicaFont.name
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }

                Row {
                    id: tsActions
                    anchors.top: chain.bottom
                    anchors.topMargin: Platform.isMobile ? 24 : 16
                    anchors.horizontalCenter: Platform.isMobile ? parent.horizontalCenter : undefined
                    anchors.left: Platform.isMobile ? undefined : chain.left
                    spacing: 12

                    Rectangle {
                        width: 130
                        height: 34
                        radius: 10
                        color: recheckArea.pressed ? theme.lowAccent : theme.lowestAccent
                        border.color: theme.lowAccent
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: diagnostics.running ? qsTr("Checking…") : qsTr("Recheck")
                            color: theme.accent
                            font.pixelSize: 13
                            font.family: geologicaFont.name
                        }
                        MouseArea {
                            id: recheckArea
                            anchors.fill: parent
                            enabled: !diagnostics.running
                            onClicked: diagnostics.runChecks(tsRoot.serverAddr)
                        }
                    }
                    Rectangle {
                        // The log is a permanent right-hand pane on desktop, so the
                        // show/hide toggle is only needed on mobile.
                        visible: Platform.isMobile
                        width: 130
                        height: 34
                        radius: 10
                        color: detailsArea.pressed ? theme.lowAccent : theme.lowestAccent
                        border.color: theme.lowAccent
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: tsRoot.showLog ? qsTr("Hide details") : qsTr("Details")
                            color: theme.accent
                            font.pixelSize: 13
                            font.family: geologicaFont.name
                        }
                        MouseArea {
                            id: detailsArea
                            anchors.fill: parent
                            onClicked: {
                                tsRoot.showLog = !tsRoot.showLog
                                if (tsRoot.showLog)
                                    logArea.text = diagnostics.logText()
                            }
                        }
                    }
                }

                Rectangle {
                    id: logPanel
                    readonly property bool tsDesktop: !Platform.isMobile
                    visible: tsRoot.showLog
                    // Positioned with explicit geometry (not anchors) so the two
                    // modes can't fight over left/right/width. Desktop: a tall pane
                    // filling the width to the right of the chain, top-aligned with
                    // it, running to the bottom of the page — so the log is always
                    // fully visible. Mobile: full-width block below the action row.
                    x: tsDesktop ? (chain.x + chain.width + 24)
                                 : (parent.width - width) / 2
                    y: tsDesktop ? chain.y : (tsActions.y + tsActions.height + 16)
                    width: tsDesktop ? Math.max(240, parent.width - x - 24)
                                     : Math.min(parent.width - 40, 520)
                    height: Math.max(0, parent.height - y - 16 - mainWindow.safeBottomInset)
                    color: theme.lowestAccent
                    border.color: theme.lowAccent
                    border.width: 1
                    radius: 10

                    Timer {
                        interval: 1000
                        repeat: true
                        running: tsRoot.showLog
                        onTriggered: logArea.text = diagnostics.logText()
                    }

                    ScrollView {
                        id: logScroll
                        anchors.fill: parent
                        anchors.margins: 10
                        anchors.bottomMargin: 48
                        clip: true
                        TextArea {
                            id: logArea
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            color: theme.text
                            font.pixelSize: 11
                            font.family: "monospace"
                            background: null
                        }
                    }

                    Row {
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 8
                        spacing: 8

                        Rectangle {
                            width: 100
                            height: 32
                            radius: 8
                            color: clearArea.pressed ? theme.lowAccent : theme.selectionAccent
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("Clear log")
                                color: theme.accent
                                font.pixelSize: 12
                                font.family: geologicaFont.name
                            }
                            MouseArea {
                                id: clearArea
                                anchors.fill: parent
                                onClicked: {
                                    diagnostics.clearLog()
                                    logArea.text = ""
                                }
                            }
                        }
                        Rectangle {
                            width: 100
                            height: 32
                            radius: 8
                            color: copyArea.pressed ? theme.lowAccent : theme.selectionAccent
                            Text {
                                anchors.centerIn: parent
                                text: copyArea.copied ? qsTr("Copied") : qsTr("Copy log")
                                color: theme.accent
                                font.pixelSize: 12
                                font.family: geologicaFont.name
                            }
                            MouseArea {
                                id: copyArea
                                property bool copied: false
                                anchors.fill: parent
                                onClicked: {
                                    clipboardHelper.setText(diagnostics.logText())
                                    copied = true
                                    copyResetTimer.restart()
                                }
                                Timer {
                                    id: copyResetTimer
                                    interval: 1500
                                    onTriggered: copyArea.copied = false
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Component { // donate page - page with donation details
        id: donatePage
        Page {
            id: donatePageRoot
            objectName: "donatePage"

            readonly property string githubUrl: "https://github.com/cathxrsys/patronus"
            readonly property int btnSize: Platform.isMobile ? 120 : 90

            // данные для открытого в данный момент QR
            property string qrName: ""
            property string qrAddress: ""
            property string qrIcon: ""

            function openQr(name, address, icon) {
                qrName = name
                qrAddress = address
                qrIcon = icon
                copiedLabel.opacity = 0
                qrPopup.open()
            }

            function copyAddress() {
                copyHelper.text = qrAddress
                copyHelper.selectAll()
                copyHelper.copy()
                copiedLabel.opacity = 1
                copiedTimer.restart()
            }

            // скрытый помощник для копирования в буфер обмена
            TextEdit {
                id: copyHelper
                visible: false
            }

            Rectangle {
                anchors.fill: parent
                color: theme.background

                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }

                Text {
                    id: donateTitleText
                    text: qsTr("Our project is free, and we need your help.")
                    color: theme.accent
                    font.pixelSize: Platform.isMobile ? 14 : 24
                    font.family: geologicaFont.name
                    font.weight: Font.Light
                    horizontalAlignment: Text.AlignHCenter
                    anchors.horizontalCenter: parent.horizontalCenter
                    wrapMode: Text.WordWrap
                    width: Platform.isMobile ? parent.width * 0.9 : 550
                    anchors.top: parent.top
                    anchors.topMargin: Platform.isMobile ? 80 : 50
                }

                // ---------- Кнопки: на мобилке столбиком, на десктопе в ряд ----------
                Grid {
                    id: buttonsRow
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: donateTitleText.bottom
                    anchors.topMargin: 50
                    columns: Platform.isMobile ? 1 : 4
                    spacing: Platform.isMobile ? 14 : 24

                    // GitHub
                    IconButton {
                        iconSource: "resources/github.svg"
                        onClicked: Qt.openUrlExternally(donatePageRoot.githubUrl)
                    }
                    // BTC
                    IconButton {
                        iconSource: "resources/btc.svg"
                        onClicked: donatePageRoot.openQr("BTC",
                            "bc1qeh4hnvfr6phf2wmvz3c65s5zssz5nzculavqc9",
                            "resources/btc.svg")
                    }
                    // ETH
                    IconButton {
                        iconSource: "resources/eth.svg"
                        onClicked: donatePageRoot.openQr("ETH",
                            "0xCd63513ec76AFf7f946D280754976f6094C6047D",
                            "resources/eth.svg")
                    }
                    // TRX
                    IconButton {
                        iconSource: "resources/trx.svg"
                        onClicked: donatePageRoot.openQr("TRX",
                            "TTbqkbQ7gPvo9fdhVhGvLq7uv55kRhZ4gA",
                            "resources/trx.svg")
                    }
                }
            }

            // ---------- Кнопка-иконка ----------
            component IconButton: Rectangle {
                id: btn
                property url iconSource
                signal clicked()

                width: donatePageRoot.btnSize
                height: donatePageRoot.btnSize
                radius: width * 0.3
                color: theme.background
                scale: mouse.pressed ? 0.92 : (mouse.containsMouse ? 1.06 : 1.0)
                Behavior on scale { NumberAnimation { duration: 100 } }

                ColorImage {
                    anchors.centerIn: parent
                    width: parent.width * 0.5
                    height: width
                    source: btn.iconSource
                    accentColor: theme.accent
                }
                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: btn.clicked()
                }
            }

            // ---------- Окно с QR ----------
            Popup {
                id: qrPopup
                modal: true
                focus: true
                anchors.centerIn: Overlay.overlay
                padding: 24
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                // затемнение фона под попапом
                Overlay.modal: Rectangle {
                    color: Qt.rgba(0, 0, 0, 0.45)   // чем больше последнее число, тем темнее
                }

                // размер QR ограничен и по ширине, и по высоте окна
                readonly property int qrSize: {
                    var maxSize = Platform.isMobile ? 260 : 280
                    var ow = Overlay.overlay ? Overlay.overlay.width : 400
                    var oh = Overlay.overlay ? Overlay.overlay.height : 700
                    var byWidth = ow - 2 * padding - 32   // 32 = запас под более широкую плашку адреса
                    var byHeight = oh - 2 * padding - 190 // место под название, адрес, "Copied" и отступы
                    return Math.max(140, Math.min(maxSize, byWidth, byHeight))
                }

                background: Rectangle {
                    color: theme.background
                    radius: 20
                }

                contentItem: Column {
                    spacing: 16

                    // карточка с QR (белая подложка нужна для надёжного сканирования)
                    Rectangle {
                        id: qrCard
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: qrPopup.qrSize
                        height: qrPopup.qrSize
                        radius: 16
                        color: "#FFFFFF"

                        Canvas {
                            id: qrCanvas
                            anchors.fill: parent
                            anchors.margins: 6
                            renderTarget: Canvas.Image
                            antialiasing: true

                            property color moduleColor: "#1A1A1A"
                            property string data: donatePageRoot.qrAddress

                            onDataChanged: requestPaint()
                            onWidthChanged: requestPaint()
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.reset()
                                if (!data || data.length === 0)
                                    return

                                var qr = QRLib.qrcode(0, "H")
                                qr.addData(data)
                                qr.make()

                                var count = qr.getModuleCount()
                                var margin = 1
                                var cell = width / (count + margin * 2)
                                var logoZone = count * 0.14
                                var mid = count / 2

                                ctx.fillStyle = moduleColor

                                for (var r = 0; r < count; r++) {
                                    for (var c = 0; c < count; c++) {
                                        if (!qr.isDark(r, c))
                                            continue
                                        if (Math.abs(r + 0.5 - mid) < logoZone &&
                                            Math.abs(c + 0.5 - mid) < logoZone)
                                            continue

                                        var x = (c + margin) * cell
                                        var y = (r + margin) * cell
                                        var inFinder =
                                            (r < 7 && c < 7) ||
                                            (r < 7 && c >= count - 7) ||
                                            (r >= count - 7 && c < 7)

                                        ctx.beginPath()
                                        if (inFinder) {
                                            var rad = cell * 0.3
                                            ctx.moveTo(x + rad, y)
                                            ctx.arcTo(x + cell, y, x + cell, y + cell, rad)
                                            ctx.arcTo(x + cell, y + cell, x, y + cell, rad)
                                            ctx.arcTo(x, y + cell, x, y, rad)
                                            ctx.arcTo(x, y, x + cell, y, rad)
                                            ctx.closePath()
                                        } else {
                                            ctx.arc(x + cell / 2, y + cell / 2, cell * 0.46, 0, Math.PI * 2)
                                        }
                                        ctx.fill()
                                    }
                                }
                            }
                        }

                        // логотип по центру QR
                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width * 0.22
                            height: width
                            radius: width * 0.28
                            color: "#FFFFFF"

                            ColorImage {
                                anchors.centerIn: parent
                                width: parent.width * 0.68
                                height: width
                                source: donatePageRoot.qrIcon
                                accentColor: theme.background
                            }
                        }
                    }

                    // название монеты
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: donatePageRoot.qrName
                        color: theme.accent
                        font.pixelSize: Platform.isMobile ? 16 : 20
                        font.family: geologicaFont.name
                        font.weight: Font.Bold
                    }

                    // адрес: по нажатию копируется
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: qrPopup.qrSize + 32
                        height: addressText.implicitHeight + 20
                        radius: 10
                        color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.1)

                        Text {
                            id: addressText
                            anchors.centerIn: parent
                            width: parent.width - 20
                            text: donatePageRoot.qrAddress
                            color: theme.accent
                            font.pixelSize: Platform.isMobile ? 11 : 13
                            font.family: geologicaFont.name
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.NoWrap
                            fontSizeMode: Text.HorizontalFit
                            minimumPixelSize: 8
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: donatePageRoot.copyAddress()
                        }
                    }

                    // место под "Copied" занято всегда: меняется только прозрачность
                    Text {
                        id: copiedLabel
                        anchors.horizontalCenter: parent.horizontalCenter
                        opacity: 0
                        text: qsTr("Copied to clipboard")
                        color: theme.accent
                        font.pixelSize: 12
                        font.family: geologicaFont.name
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                    }
                    Timer {
                        id: copiedTimer
                        interval: 1500
                        onTriggered: copiedLabel.opacity = 0
                    }
                }
            }
        }
    }
    Component { // updatesPage - page with update information and controls
        id: updatesPage
        Page {
            objectName: "updatesPage"

            Component.onCompleted: UpdateManager.refresh()

            Rectangle {
                anchors.fill: parent
                color: theme.background

                BackButton {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: Platform.isMobile ? 20 : 24
                    anchors.leftMargin: Platform.isMobile ? 16 : 20
                }

                ColumnLayout {
                    anchors.top: parent.top
                    anchors.topMargin: Platform.isMobile ? 74 : 64
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Platform.isMobile ? (parent.width - 36) : Math.min(parent.width - 64, 700)
                    spacing: 8

                        Text {
                            text: qsTr("Updates")
                            color: theme.accent
                            font.pixelSize: Platform.isMobile ? 22 : 28
                            font.family: geologicaFont.name
                            font.weight: Font.Bold
                            Layout.fillWidth: true
                        }

                        Text {
                            text: qsTr("Current version: %1").arg(UpdateManager.currentVersion)
                                 + "\n" + qsTr("Current protocol version: %1").arg(UpdateManager.currentProtocolVersion)
                            color: theme.halfAccent
                            font.pixelSize: Platform.isMobile ? 12 : 14
                            font.family: geologicaFont.name
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        RightBarButton {
                            Layout.fillWidth: true
                            icon.source: "resources/restore.svg"
                            buttonColor: theme.accent
                            backgroundColor: theme.background
                            text: UpdateManager.checking ? qsTr("Checking...") : qsTr("Check for updates")
                            enabled: !UpdateManager.checking
                            font.pixelSize: 12
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            onClicked: UpdateManager.refresh()
                        }

                        SettingsGroup {
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Status")
                            Layout.fillWidth: true
                        }

                        Text {
                            Layout.fillWidth: true
                            text: UpdateManager.statusText
                            color: UpdateManager.updateAvailable ? theme.yellowAccent : theme.accent
                            font.pixelSize: Platform.isMobile ? 12 : 13
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            visible: !UpdateManager.repositoryConfigured
                            text: qsTr("Set the GitHub repository in Settings to enable update checks. Use owner/repository format.")
                            color: theme.halfAccent
                            font.pixelSize: Platform.isMobile ? 12 : 13
                            font.family: geologicaFont.name
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Text {
                            visible: UpdateManager.repositoryConfigured
                            text: qsTr("Repository: %1").arg(UpdateManager.repository)
                            color: theme.halfAccent
                            font.pixelSize: Platform.isMobile ? 12 : 13
                            font.family: geologicaFont.name
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Text {
                            visible: UpdateManager.latestVersion.length > 0
                            text: qsTr("Latest version: %1").arg(UpdateManager.latestVersion)
                            color: theme.accent
                            font.pixelSize: Platform.isMobile ? 13 : 14
                            font.family: geologicaFont.name
                            font.weight: Font.Medium
                            Layout.fillWidth: true
                        }

                        Text {
                            visible: UpdateManager.latestProtocolVersion.length > 0
                            text: qsTr("Latest protocol version: %1").arg(UpdateManager.latestProtocolVersion)
                            color: theme.halfAccent
                            font.pixelSize: Platform.isMobile ? 12 : 13
                            font.family: geologicaFont.name
                            Layout.fillWidth: true
                        }

                        Rectangle {
                            visible: UpdateManager.protocolMismatch
                            Layout.fillWidth: true
                            implicitHeight: protocolWarningText.implicitHeight + 24
                            color: theme.selectionAccent
                            border.color: theme.yellowAccent
                            border.width: 1

                            Text {
                                id: protocolWarningText
                                anchors.fill: parent
                                anchors.margins: 12
                                text: qsTr("Protocol versions differ. After installing the new version, you may lose connectivity to servers that still use a different protocol version.")
                                color: theme.accent
                                font.pixelSize: Platform.isMobile ? 12 : 13
                                font.family: geologicaFont.name
                                wrapMode: Text.WordWrap
                            }
                        }

                        SettingsGroup {
                            visible: UpdateManager.latestVersion.length > 0
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Release Notes")
                            Layout.fillWidth: true
                        }

                        Text {
                            visible: UpdateManager.releaseNotes.length > 0
                            text: UpdateManager.releaseNotes
                            color: theme.halfAccent
                            font.pixelSize: Platform.isMobile ? 12 : 13
                            font.family: geologicaFont.name
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        SettingsGroup {
                            visible: UpdateManager.updateAvailable || UpdateManager.downloadInProgress || UpdateManager.downloadReady
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Download")
                            Layout.fillWidth: true
                        }

                        ProgressBar {
                            visible: UpdateManager.downloadInProgress || UpdateManager.downloadReady
                            Layout.fillWidth: true
                            from: 0
                            to: 1
                            value: UpdateManager.downloadReady ? 1 : UpdateManager.downloadProgress
                            indeterminate: UpdateManager.downloadInProgress && UpdateManager.downloadBytesTotal <= 0
                        }

                        Text {
                            visible: UpdateManager.downloadInProgress || UpdateManager.downloadReady
                            text: UpdateManager.downloadReady
                                ? qsTr("Downloaded file: %1").arg(UpdateManager.downloadedFileName)
                                : qsTr("Downloaded %1 of %2")
                                    .arg(mainWindow.formatFileSize(UpdateManager.downloadBytesReceived))
                                    .arg(UpdateManager.downloadBytesTotal > 0
                                        ? mainWindow.formatFileSize(UpdateManager.downloadBytesTotal)
                                        : qsTr("unknown"))
                            color: theme.halfAccent
                            font.pixelSize: Platform.isMobile ? 12 : 13
                            font.family: geologicaFont.name
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        ActionButton {
                            visible: UpdateManager.updateAvailable && !UpdateManager.downloadReady
                            enabled: !UpdateManager.downloadInProgress && UpdateManager.repositoryConfigured
                            text: UpdateManager.downloadInProgress ? qsTr("Downloading update") : qsTr("Download update")
                            buttonColor: theme.accent
                            textColor: theme.background
                            Layout.preferredHeight: 44
                            Layout.fillWidth: true
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            font.pixelSize: 12
                            onClicked: UpdateManager.downloadUpdate()
                        }

                        ActionButton {
                            visible: UpdateManager.downloadReady
                            text: qsTr("Install update")
                            buttonColor: theme.yellowAccent
                            textColor: theme.background
                            Layout.preferredHeight: 44
                            Layout.fillWidth: true
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            font.pixelSize: 12
                            onClicked: UpdateManager.installUpdate()
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 20
                            color: "transparent"
                        }
                }
            }
        }
    }
    Component { // password page - page for entering password to open database, if it is encrypted
        id: passwordPage
        Page {
            objectName: "passwordPage"
            Rectangle {
                anchors.fill: parent
                color: theme.background
                ColumnLayout {
                    anchors.margins: 0
                    spacing: 20
                    width: parent.width
                    height: 200
                    anchors.centerIn: parent
                    Text { // Enter your password
                        text: qsTr("Enter your password")
                        color: theme.accent
                        font.pixelSize: 20
                        font.family: geologicaFont.name
                        font.weight: Font.Bold
                        horizontalAlignment: Text.AlignHCenter
                        Layout.alignment: Qt.AlignHCenter
                        Layout.bottomMargin: 0
                        Layout.fillWidth: true
                    }
                    GradientLine {
                        Layout.preferredWidth: 200
                        height: 2
                        colorSide: "transparent"
                        colorOutSide: theme.lowAccent
                        colorCenter: theme.accent
                        Layout.alignment: Qt.AlignHCenter
                    }
                    TextField { // password input
                        id: passwordField
                        Layout.preferredWidth: 300
                        echoMode: TextInput.Password
                        inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                        selectedTextColor: theme.background
                        font.pixelSize: 12
                        font.family: geologicaFont.name
                        color: theme.accent
                        placeholderText: qsTr("Password")
                        placeholderTextColor: theme.lowAccent
                        Layout.alignment: Qt.AlignHCenter
                        background: Rectangle {
                            color: theme.lowestAccent
                            radius: 4
                        }
                        Keys.onReturnPressed: {
                            if (passwordField.text.trim().length > 0) {
                                submitButton.clicked()
                            }
                        }
                    }
                    ActionButton { // submit button
                        id: submitButton
                        text: qsTr("Continue")
                        buttonColor: theme.accent
                        textColor: theme.background
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        enabled: passwordField.text.trim().length > 0
                        Layout.alignment: Qt.AlignHCenter
                        onClicked: { // !
                            console.log("Password entered")
                            submitButton.text = qsTr("please wait...")
                            if (AppSession.openWithPassword(passwordField.text.trim())) {
                                console.log("Password correct, proceeding");
                                errorText.visible = false
                                passwordField.text = ""
                                stack.push(mainPage)
                            } else {
                                console.log("Incorrect password: " + AppSession.lastError);
                                passwordField.text = ""
                                submitButton.text = qsTr("continue")
                                errorText.visible = true
                            }
                        }
                    }
                    Text { // error text
                        id: errorText
                        text: qsTr("Incorrect password, try again.")
                        color: theme.redAccent
                        font.pixelSize: 12
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        horizontalAlignment: Text.AlignHCenter
                        Layout.alignment: Qt.AlignHCenter
                        Layout.bottomMargin: 0
                        Layout.fillWidth: true
                        visible: false
                    }
                }
            }
        }
    }
    Component { // change password page - page for changing or removing password of the database when it is already open
        id: changePasswordPage
        Page {
            objectName: "changePasswordPage"
            Rectangle {
                anchors.fill: parent
                color: theme.background
                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }
                ColumnLayout {
                    anchors.margins: 0
                    spacing: 20
                    width: parent.width
                    height: 200
                    anchors.centerIn: parent
                    Text {
                        text: qsTr("Enter new password")
                        color: theme.accent
                        font.pixelSize: 20
                        font.family: geologicaFont.name
                        font.weight: Font.Bold
                        horizontalAlignment: Text.AlignHCenter
                        Layout.alignment: Qt.AlignHCenter
                        Layout.bottomMargin: 0
                        Layout.fillWidth: true
                    }
                    Text {
                        text: qsTr("Please enter a new password. Make sure you remember it, because if you lose your password, it cannot be recovered.")
                        color: theme.lowAccent
                        font.pixelSize: 12
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        horizontalAlignment: Text.AlignHCenter
                        Layout.alignment: Qt.AlignHCenter
                        Layout.bottomMargin: 0
                        Layout.preferredWidth: 300
                        wrapMode: Text.WordWrap
                    }
                    GradientLine {
                        Layout.preferredWidth: 200
                        height: 2
                        colorSide: "transparent"
                        colorOutSide: theme.lowAccent
                        colorCenter: theme.accent
                        Layout.alignment: Qt.AlignHCenter
                    }
                    TextField {
                        id: passwordField
                        Layout.preferredWidth: 300
                        echoMode: TextInput.Password
                        inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                        font.pixelSize: 12
                        font.family: geologicaFont.name
                        color: theme.accent
                        placeholderText: qsTr("New password")
                        placeholderTextColor: theme.lowAccent
                        Layout.alignment: Qt.AlignHCenter
                        background: Rectangle {
                            color: theme.lowestAccent
                            radius: 4
                        }
                        Keys.onReturnPressed: {
                            if (passwordField.text.trim().length > 0) {
                                submitButton.clicked()
                            }
                        }
                    }
                    RowLayout { // !
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 10
                        ActionButton {
                            text: qsTr("remove")
                            buttonColor: theme.redAccent
                            textColor: theme.background
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            onClicked: {
                                console.log("Remove button clicked");
                                if (AppSession.changeDatabasePassword(DEFAULT_DB_PASSWORD)) {
                                    console.log("Password removed successfully");
                                    submitButton.text = qsTr("remove")
                                    passwordField.text = ""
                                    stack.pop();
                                } else {
                                    console.log("Failed to remove password: " + AppSession.lastError);
                                    passwordField.text = ""
                                    submitButton.text = qsTr("remove")
                                }
                            }
                        }
                        ActionButton {
                            id: submitButton
                            text: qsTr("continue")
                            buttonColor: theme.accent
                            textColor: theme.background
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            enabled: passwordField.text.trim().length > 0
                            onClicked: {
                                var newPassword = passwordField.text.trim();
                                console.log("New password entered");
                                submitButton.text = qsTr("please wait...")
                                if (newPassword.length == 0) {
                                    console.log("Password empty, set default password");
                                    newPassword = DEFAULT_DB_PASSWORD;
                                }
                                if (AppSession.changeDatabasePassword(newPassword)) {
                                    console.log("Password changed successfully");
                                    submitButton.text = qsTr("continue")    
                                    passwordField.text = ""
                                    stack.pop();
                                } else {
                                    console.log("Failed to change password: " + AppSession.lastError);
                                    passwordField.text = ""
                                    submitButton.text = qsTr("continue")
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    Component { // mainPage
        id: mainPage
        Page {
            objectName: "mainPage"
            id: mainPagePage
            property int sideBarWidth: 300
            property int rightBarWidth: 350
            readonly property bool isMobileLayout: Platform.isMobile
            readonly property int mobilePaneChats: 0
            readonly property int mobilePaneChat: 1
            readonly property int mobilePaneInfo: 2
            property int mobilePane: mobilePaneChats
            // Exposed so the root-level bottom nav bar (which overlays the whole
            // StackView) can tell when this page is showing the chats list.
            readonly property bool atChatsList: mobilePane === mobilePaneChats
            property var mobileNavigationHistory: []
            property string homeServerAddress: Settings.getTextSetting("serverAddress", "")
            property bool homeServerConnected: false
            // Home-server lifecycle phase (see ConnectionManager::connectionPhase):
            // 0 not connected · 2 authenticating · 3 synchronizing · 4 ready.
            property int homeServerPhase: 0

            function refreshHomeServerStatus() {
                homeServerAddress = Settings.getTextSetting("serverAddress", "")
                homeServerPhase = homeServerAddress.length > 0
                    ? connections.connectionPhase(homeServerAddress) : 0
                // Green the moment we're authenticated (phase >= 3), i.e. before
                // the offline backlog has finished draining.
                homeServerConnected = homeServerPhase >= 3
            }

            Component.onCompleted: refreshHomeServerStatus()

            Connections {
                target: Settings
                function onSettingChanged(settingName, value) {
                    if (settingName === "serverAddress")
                        mainPagePage.refreshHomeServerStatus()
                }
            }

            Connections {
                target: connections
                function onConnectionStatusChanged(serverId, status) {
                    if (serverId === mainPagePage.homeServerAddress)
                        mainPagePage.refreshHomeServerStatus()
                }

                // Handshake done -> green indicator + "Synchronizing…" title,
                // without waiting for the backlog to finish.
                function onAuthenticated(serverId) {
                    if (serverId === mainPagePage.homeServerAddress)
                        mainPagePage.refreshHomeServerStatus()
                }

                function onAuthChanged(serverId, success) {
                    if (serverId === mainPagePage.homeServerAddress)
                        mainPagePage.refreshHomeServerStatus()
                }
            }

            function openChat(index, server, pubkeyFingerprint, unread) {
                const hadUnreadMessages = unread > 0
                replyDraft.clear()
                attachmentStaging.clear()
                chats.currentIndex = index
                messageModel.loadFromDatabase(server, pubkeyFingerprint)
                mainChatContainer.followBottomAfterOpen()
                if (hadUnreadMessages) {
                    connections.sendReadedSignal(server, pubkeyFingerprint)
                    MainSignals.emitAllMessagesReaded(server, pubkeyFingerprint)
                    // Reading the chat: drop the message notification(s) from the
                    // tray so the user doesn't have to swipe them away by hand.
                    // Notifications carry no per-chat id (the push is E2E-blind)
                    // and all collapse into one tray entry, so this clears "the"
                    // message notification. A live incoming call is left alone.
                    if (Platform.isAndroid && AndroidSystemUi.available)
                        AndroidSystemUi.clearMessageNotifications()
                }
                if (isMobileLayout)
                    showChatPane(true)
            }

            function closeCurrentChat() {
                if (chats.currentIndex === -1)
                    return false

                replyDraft.clear()
                attachmentStaging.clear()
                messageModel.closeCurrentChat()
                chats.currentIndex = -1
                return true
            }

            function setMobilePane(targetPane, rememberCurrent) {
                if (!isMobileLayout || targetPane === mobilePane)
                    return false

                let nextHistory = mobileNavigationHistory.slice()
                if (rememberCurrent)
                    nextHistory.push(mobilePane)

                mobileNavigationHistory = nextHistory
                mobilePane = targetPane
                return true
            }

            function resetMobileNavigation(targetPane) {
                mobileNavigationHistory = []
                mobilePane = targetPane
            }

            function canNavigateMobileBack() {
                if (!isMobileLayout)
                    return false

                return mobileNavigationHistory.length > 0 || mobilePane !== mobilePaneChats
            }

            // Back/Escape first dismisses the docked emoji panel (if open) before
            // it navigates panes. The panel is a Popup parented to the window's
            // contentItem (not this chat pane), so without this a back press would
            // navigate the chat away and leave the panel floating over the pane
            // behind it. Returns true when it consumed the press.
            function closeEmojiPickerIfOpen() {
                if (emojiPicker.visible) {
                    emojiPicker.close()
                    return true
                }
                return false
            }

            function navigateMobileBack() {
                if (!isMobileLayout)
                    return false

                let nextHistory = mobileNavigationHistory.slice()
                while (nextHistory.length > 0) {
                    const previousPane = nextHistory.pop()
                    if (previousPane !== mobilePaneInfo || chats.currentIndex !== -1) {
                        if (previousPane !== mobilePaneChat || chats.currentIndex !== -1) {
                            if (previousPane === mobilePaneChats) {
                                closeCurrentChat()
                                mobileNavigationHistory = []
                            } else {
                                mobileNavigationHistory = nextHistory
                            }
                            mobilePane = previousPane
                            return true
                        }
                    }
                }

                if (mobilePane !== mobilePaneChats) {
                    closeCurrentChat()
                    mobileNavigationHistory = []
                    mobilePane = mobilePaneChats
                    return true
                }

                return false
            }

            function showChatsPane(rememberCurrent) {
                if (!isMobileLayout)
                    return

                closeCurrentChat()
                mobileNavigationHistory = []
                mobilePane = mobilePaneChats
            }

            function showChatPane(rememberCurrent) {
                if (chats.currentIndex !== -1)
                    setMobilePane(mobilePaneChat, rememberCurrent === undefined ? mobilePane !== mobilePaneChat : rememberCurrent)
            }

            function showInfoPane(rememberCurrent) {
                if (chats.currentIndex !== -1)
                    setMobilePane(mobilePaneInfo, rememberCurrent === undefined ? mobilePane !== mobilePaneInfo : rememberCurrent)
            }

            Rectangle {
                anchors.fill: parent
                color: theme.background
                RowLayout {
                    anchors.fill: parent
                    spacing: 0
                    ColumnLayout { // sideBar
                        id: sideBar
                        visible: !mainPagePage.isMobileLayout || mainPagePage.mobilePane === mainPagePage.mobilePaneChats
                        Layout.preferredWidth: mainPagePage.isMobileLayout ? parent.width : mainPagePage.sideBarWidth
                        Layout.minimumWidth: mainPagePage.isMobileLayout ? 0 : mainPagePage.sideBarWidth
                        Layout.maximumWidth: mainPagePage.isMobileLayout ? parent.width : mainPagePage.sideBarWidth
                        Layout.preferredHeight: parent.height
                        Layout.maximumHeight: parent.height
                        Layout.alignment: Qt.AlignLeft
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Platform.isMobile ? 36 : 50
                            z: 350
                            color: theme.background
                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 0
                                spacing: 0
                                Text { // Left sidebar title: app name, or sync status while the backlog drains
                                    text: mainPagePage.homeServerPhase === 3
                                          ? qsTr("Synchronizing...") : qsTr("Patronus")
                                    Layout.alignment: Qt.AlignLeft | (Platform.isMobile ? Qt.AlignTop : Qt.AlignVCenter)
                                    Layout.leftMargin: 12
                                    color: theme.accent
                                    font.pixelSize: 25
                                    font.family: geologicaFont.name
                                    font.weight: Font.Medium
                                }                          

                                Rectangle {
                                    visible: Platform.isMobile
                                    Layout.alignment: Qt.AlignVCenter
                                    Layout.leftMargin: 10
                                    Layout.preferredWidth: 10
                                    Layout.preferredHeight: 10
                                    radius: 5
                                    antialiasing: true
                                    layer.enabled: true
                                    layer.smooth: true
                                    color: mainPagePage.homeServerConnected ? "transparent" : "#05FFFFFF"
                                    gradient: mainPagePage.homeServerConnected ? homeServerIndicatorGradientMobile : null

                                    Gradient {
                                        id: homeServerIndicatorGradientMobile
                                        orientation: Gradient.Horizontal
                                        GradientStop { position: 0.0; color: "#00c4dc" }
                                        GradientStop { position: 1.0; color: "#00ef96" }
                                    }
                                }

                                

                                Item {
                                    Layout.fillWidth: true
                                }
                                BottomButton {
                                    id: chatsMenuButton
                                    visible: mainPagePage.isMobileLayout
                                    Layout.alignment: (Platform.isMobile ? Qt.AlignTop : Qt.AlignVCenter)
                                    Layout.preferredWidth: 44
                                    Layout.preferredHeight: 36
                                    icon.source: "resources/menu.svg"
                                    icon.color: theme.accent
                                    icon.width: 20
                                    icon.height: 20
                                    onClicked: {
                                        chatsHeaderMenu.x = Math.max(0, chatsMenuButton.x + chatsMenuButton.width - chatsHeaderMenu.width)
                                        chatsHeaderMenu.y = chatsMenuButton.y + chatsMenuButton.height
                                        chatsHeaderMenu.open()
                                    }
                                }
                                Rectangle {
                                    Layout.preferredWidth: 10
                                    Layout.fillHeight: true
                                    color: "transparent"
                                    visible: Platform.isMobile
                                }
                                Menu {
                                    id: chatsHeaderMenu
                                    topInset: 0
                                    bottomInset: 0
                                    padding: 0
                                    margins: 0
                                    background: Rectangle {
                                        implicitWidth: 160
                                        color: mainWindow.chatPanelSurface
                                        border.color: mainWindow.chatPanelBorderColor
                                        border.width: 1
                                        radius: 14

                                        PanelBorder {
                                            edgeColor: mainWindow.chatPanelBorderColor
                                        }
                                    }
                                    MyMenuItem {
                                        text: qsTr("Settings")
                                        leftAlign: true
                                        colorAccent: theme.accent
                                        colorBackground: theme.background
                                        colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                        onTriggered: stack.push(settingsPage)
                                        iconSource: "resources/settings.svg"
                                    }
                                    MyMenuItem {
                                        text: qsTr("Updates")
                                        leftAlign: true
                                        colorAccent: theme.accent
                                        colorBackground: theme.background
                                        colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                        onTriggered: stack.push(updatesPage)
                                        iconSource: "resources/update.svg"
                                    }
                                    MyMenuItem {
                                        text: qsTr("Donate")
                                        leftAlign: true
                                        colorAccent: theme.accent
                                        colorBackground: theme.background
                                        colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                        onTriggered: stack.push(donatePage)
                                        iconSource: "resources/donate.svg"
                                    }
                                    MyMenuItem {
                                        text: qsTr("Add contact")
                                        leftAlign: true
                                        colorAccent: theme.accent
                                        colorBackground: theme.background
                                        colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                        onTriggered: stack.push(addContactPage)
                                        iconSource: "resources/add_contact.svg"
                                    }
                                    MyMenuItem {
                                        text: qsTr("My QR")
                                        leftAlign: true
                                        colorAccent: theme.accent
                                        colorBackground: theme.background
                                        colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                        onTriggered: stack.push(qrCodePage)
                                        iconSource: "resources/qr.svg"
                                    }
                                    MyMenuItem {
                                        text: qsTr("Troubleshooting")
                                        leftAlign: true
                                        colorAccent: theme.accent
                                        colorBackground: theme.background
                                        colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                        onTriggered: stack.push(troubleshootingPage)
                                        iconSource: "resources/internet.svg"
                                    }
                                    MyMenuItem {
                                        text: qsTr("I've been hacked")
                                        leftAlign: true
                                        isRed: true
                                        colorRedAccent: theme.redAccent
                                        colorBackground: theme.background
                                        colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                        onTriggered: stack.push(hackedPage)
                                        iconSource: "resources/hacker.svg"
                                    }
                                }
                            }
                        }
                        ListView { // chats list
                            id: chats
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            // Trailing space so the last chat can scroll clear of the
                            // floating bottom nav bar (mobile) instead of hiding behind it.
                            bottomMargin: mainPagePage.isMobileLayout ? 92 : 0
                            model: contactsModel
                            currentIndex: -1
                            // A new message bumps its contact to the top of the
                            // list. QQuickListView does NOT move currentIndex with
                            // a relocated row, so pin the open chat here — otherwise
                            // currentIndex would silently point at the wrong contact
                            // (wrong header, and messages sent to the wrong person).
                            Connections {
                                target: contactsModel
                                function onContactMoved(from, to) {
                                    const cur = chats.currentIndex
                                    if (cur < 0 || from === to)
                                        return
                                    if (cur === from)
                                        chats.currentIndex = to
                                    else if (from < to && cur > from && cur <= to)
                                        chats.currentIndex = cur - 1
                                    else if (from > to && cur >= to && cur < from)
                                        chats.currentIndex = cur + 1
                                }
                            }
                            delegate: ItemDelegate {
                                id: chatDelegate
                                width: chats.width
                                height: 72
                                property bool serverConnected: server.length > 0 ? connections.isConnected(server) : false
                                background: Rectangle {
                                    color: mainPagePage.isMobileLayout
                                        ? (chatMouseArea.pressed ? theme.selectionAccent : "transparent")
                                        : (chats.currentIndex === index ? theme.lowestAccent
                                            : (parent.hovered ? theme.selectionAccent : "transparent"))
                                }
                                Menu { //  optionsMenu3 - menu for each chat in the list
                                    id: optionsMenu3
                                    y: parent.height
                                    topInset: 0
                                    bottomInset: 0
                                    padding: 0
                                    property bool leftAlign: false
                                    property bool isRed: false
                                    // Identify the target by stable (server, pubkey)
                                    // rather than row index: a message can bump a
                                    // contact to the top while this menu is open,
                                    // shifting indices out from under it.
                                    property string contactServer: ""
                                    property string contactPubKey: ""
                                    background: Rectangle {
                                        implicitWidth: 150
                                        color: mainWindow.chatPanelSurface
                                        border.color: mainWindow.chatPanelBorderColor
                                        border.width: 1
                                        radius: 14

                                        PanelBorder {
                                            edgeColor: mainWindow.chatPanelBorderColor
                                        }
                                    }
                                    MyMenuItem { // removeContactMenuItem
                                        text: qsTr("Remove")
                                        onTriggered: {
                                            if (optionsMenu3.contactPubKey.length === 0)
                                                return

                                            MainSignals.emitContactRemove(
                                                optionsMenu3.contactServer,
                                                optionsMenu3.contactPubKey
                                            )
                                        }
                                        leftAlign: optionsMenu3.leftAlign
                                        isRed: true
                                        colorAccent: theme.accent
                                        colorBackground: theme.background
                                        colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                        iconSource: "resources/remove.svg"
                                    }
                                }
                                MouseArea { // chatMouseArea
                                    id: chatMouseArea
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    acceptedButtons: Qt.RightButton | Qt.LeftButton
                                    onClicked: (mouse) => { // !!!
                                        if (mouse.button === Qt.RightButton) {
                                            optionsMenu3.leftAlign = true
                                            optionsMenu3.isRed = false
                                            optionsMenu3.contactServer = server
                                            optionsMenu3.contactPubKey = pubkeyFingerprint
                                            optionsMenu3.popup(mouse.x, mouse.y)
                                            return;
                                        } else if (mouse.button == Qt.LeftButton) {
                                            mainPagePage.openChat(index, server, pubkeyFingerprint, unread)
                                        }
                                    }
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    spacing: 10
                                    Rectangle { // avatar
                                        id: avatarContainer
                                        Layout.preferredWidth: 52
                                        Layout.preferredHeight: 52
                                        Layout.alignment: Qt.AlignVCenter
                                        radius: width / 2
                                        color: theme.minimumAccent
                                        layer.enabled: true
                                        Text { // avatar letter
                                            id: avatarLetter
                                            anchors.centerIn: parent
                                            text: avatar
                                            font.pixelSize: 28
                                            font.family: geologicaFont.name
                                            color: theme.accent
                                            // Bound to the reactive `avatarSource` model role (not the
                                            // one-shot Account.hasContactAvatar() call) so it flips off
                                            // the moment a freshly-added contact's avatar arrives —
                                            // updateContact() sets avatarSource to an image://avatars/
                                            // URL and emits dataChanged, re-evaluating this.
                                            visible: !avatarSource.startsWith("image://avatars/")
                                        }
                                        Image { // avatar image
                                            id: avatarImg
                                            anchors.fill: parent
                                            visible: false
                                            source: Account.getContactAvatarSource(server, pubkeyFingerprint)
                                            mipmap: true
                                            fillMode: Image.PreserveAspectCrop
                                            smooth: true
                                            antialiasing: true
                                            layer.enabled: true
                                            layer.smooth: true
                                        }
                                        Connections { // !
                                            target: connections
                                            function onContactAvatarChanged(updatedServerId, updatedPubKeyFingerprint) {
                                                if (updatedServerId === server && updatedPubKeyFingerprint === pubkeyFingerprint) {
                                                    avatarImg.source = Account.getContactAvatarSource(server, pubkeyFingerprint) + "?t=" + Date.now();
                                                }
                                            }
                                        }
                                        Rectangle { // avatar mask
                                            id: mask
                                            anchors.fill: parent
                                            radius: avatarContainer.radius
                                            visible: false
                                            layer.enabled: true
                                            layer.smooth: true
                                        }
                                        MultiEffect { // avatar circle effect
                                            anchors.fill: parent
                                            source: avatarImg
                                            maskEnabled: true
                                            maskSource: mask
                                            // See avatarLetter above: reactive `avatarSource` role so the
                                            // masked image appears as soon as the avatar is received.
                                            visible: avatarSource.startsWith("image://avatars/")
                                            smooth: true
                                            antialiasing: true
                                            maskSpreadAtMin: 1.0
                                            maskThresholdMin: 0.6
                                        }
                                        Rectangle {
                                            visible: isOnline || !serverConnected
                                            width: 14
                                            height: 14
                                            radius: 7
                                            antialiasing: true
                                            // Online reuses the server-connection dot's
                                            // cyan→green gradient; offline stays red.
                                            color: isOnline ? "transparent" : theme.redAccent
                                            gradient: isOnline ? onlineDotGradient : null
                                            border.color: theme.background
                                            border.width: 2
                                            anchors.right: parent.right
                                            anchors.bottom: parent.bottom
                                            Gradient {
                                                id: onlineDotGradient
                                                orientation: Gradient.Horizontal
                                                GradientStop { position: 0.0; color: "#00c4dc" }
                                                GradientStop { position: 1.0; color: "#00ef96" }
                                            }
                                        }
                                        Connections {
                                            target: connections
                                            function onConnectionStatusChanged(serverId, status) {
                                                if (serverId === server) {
                                                    chatDelegate.serverConnected = connections.isConnected(server)
                                                }
                                            }
                                        }
                                    } // avatar
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        Layout.alignment: Qt.AlignVCenter
                                        Layout.margins: 2
                                        spacing: 0
                                        // No clip here: it would cut the unique name's glow at the
                                        // avatar side. Children still elide, so nothing overflows.
                                        RowLayout {
                                            StyledName { // firstname + lastname
                                                firstName: model.firstName
                                                lastName: model.lastName
                                                styleCss: model.nameStyle
                                                fallbackColor: theme.halfAccent
                                                pixelSize: 14
                                                fontFamily: "Roboto"
                                                fontWeight: Font.Medium
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: implicitHeight
                                                elide: Text.ElideRight
                                            }
                                            Rectangle { // HACKED badge - contact's key was revoked as compromised
                                                visible: model.revoked === true
                                                Layout.alignment: Qt.AlignVCenter
                                                Layout.preferredHeight: 15
                                                Layout.preferredWidth: hackedBadgeText.implicitWidth + 10
                                                radius: 3
                                                color: theme.redAccent
                                                Text {
                                                    id: hackedBadgeText
                                                    anchors.centerIn: parent
                                                    text: qsTr("HACKED")
                                                    color: theme.background
                                                    font.pixelSize: 9
                                                    font.family: "Roboto"
                                                    font.weight: Font.Bold
                                                }
                                            }
                                            Image { // checked
                                                source: "resources/checked.svg"
                                                Layout.preferredHeight: 12
                                                Layout.preferredWidth: height
                                                mipmap: true
                                                fillMode: Image.PreserveAspectFit
                                                smooth: true
                                                antialiasing: true
                                                visible: false
                                            }
                                            Image { // double checked
                                                source: "resources/double_checked.svg"
                                                Layout.preferredHeight: 12
                                                Layout.preferredWidth: height
                                                mipmap: true
                                                fillMode: Image.PreserveAspectFit
                                                smooth: true
                                                antialiasing: true
                                                visible: false
                                            }
                                            Text { // time
                                                text: time
                                                color: theme.halfAccent
                                                font.pixelSize: 10
                                                font.family: "Roboto"
                                                font.weight: Font.Light
                                                opacity: 0.6
                                            }
                                        }
                                        RowLayout {
                                            Text { // last message
                                                text: mainWindow.callStatusText(lastMessage)
                                                color: theme.halfAccent
                                                font: Fonts.emoji("Roboto", 12, Font.Light)
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                                wrapMode: Text.WordWrap
                                                maximumLineCount: 2
                                                opacity: 0.6
                                                lineHeight: 0.9
                                            }
                                            Rectangle { // unread messages counter
                                                Layout.alignment: Qt.AlignRight | Qt.AlignBottom
                                                Layout.preferredWidth: 18
                                                Layout.preferredHeight: 18
                                                visible: unread > 0
                                                radius: width / 2
                                                color: theme.accent
                                                Text {
                                                    anchors.fill: parent
                                                    anchors.centerIn: parent
                                                    horizontalAlignment: Text.AlignHCenter
                                                    verticalAlignment: Text.AlignVCenter
                                                    text: unread > 99 ? "99+" : unread
                                                    color: theme.background
                                                    font.pixelSize: 9
                                                    font.family: "Roboto"
                                                    font.weight: Font.Medium
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            footer: ItemDelegate {
                                width: chats.width
                                height: 72
                                background: Rectangle {
                                    color: parent.hovered ? theme.selectionAccent : "transparent"
                                }
                                HoverHandler {
                                    cursorShape: Qt.PointingHandCursor
                                }
                                onClicked: stack.push(addContactPage)
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    spacing: 10
                                    Rectangle {
                                        Layout.preferredWidth: 52
                                        Layout.preferredHeight: 52
                                        Layout.alignment: Qt.AlignVCenter
                                        radius: width / 2
                                        color: theme.minimumAccent
                                        layer.enabled: true
                                        ColorImage {
                                            anchors.centerIn: parent
                                            width: 20
                                            height: 20
                                            source: "resources/add_contact.svg"
                                            accentColor: theme.accent
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        Layout.alignment: Qt.AlignVCenter
                                        Layout.margins: 2
                                        spacing: 0
                                        clip: true
                                        Text {
                                            text: qsTr("Add contact")
                                            color: theme.halfAccent
                                            font.pixelSize: 14
                                            font.family: "Roboto"
                                            font.weight: Font.Medium
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: qsTr("Start a new conversation")
                                            color: theme.halfAccent
                                            font.pixelSize: 12
                                            font.family: "Roboto"
                                            font.weight: Font.Light
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                            opacity: 0.6
                                        }
                                    }
                                }
                            }
                            ScrollBar.vertical: ScrollBar {
                                policy: ScrollBar.AsNeeded // or ScrollBar.AlwaysOn
                                width: 6
                                minimumSize: 0.1
                                interactive: true
                                contentItem: Rectangle {
                                    radius: 5
                                    color: theme.accent
                                    opacity: 0.2
                                }
                                background: Rectangle {
                                    radius: 0
                                    color: "transparent"
                                    opacity: 0.1
                                }
                            }
                        }
                        Connections { // !
                            target: MainSignals
                            function onContactRemovedIndex(index) {
                                console.log("Contact removed at index: " + index)

                                if (chats.currentIndex === index) {
                                    mainPagePage.closeCurrentChat()
                                    if (mainPagePage.isMobileLayout)
                                        mainPagePage.resetMobileNavigation(mainPagePage.mobilePaneChats)
                                }
                            }
                        }
                        Connections {
                            target: chats
                            function onCurrentIndexChanged() {
                                if (mainPagePage.isMobileLayout && chats.currentIndex === -1)
                                    mainPagePage.resetMobileNavigation(mainPagePage.mobilePaneChats)
                            }
                        }
                    } // sideBar
                    Rectangle { // sideBarResizer
                        id: sideBarResizer
                        visible: !mainPagePage.isMobileLayout
                        Layout.preferredWidth: visible ? 2 : 0
                        Layout.minimumWidth: visible ? 2 : 0
                        Layout.maximumWidth: visible ? 2 : 0
                        Layout.fillHeight: true
                        Layout.alignment: Qt.AlignRight
                        color: "transparent"
                        opacity: 0.1
                        MouseArea {
                            cursorShape: Qt.SizeHorCursor
                            height: parent.height
                            anchors.centerIn: parent
                            preventStealing: true
                            width: pressed ? 2000 : parent.width
                            property real startX
                            property real startWidth
                            onPressed: {
                                startX = mapToItem(null, mouse.x, 0). x
                                startWidth = mainPagePage. sideBarWidth
                            }
                            onPositionChanged: {
                                var mouseX = mapToItem(null, mouse.x, 0).x
                                let newWidth = startWidth + (mouseX - startX)
                                newWidth = Math.max(250, Math.min(600, newWidth))
                                mainPagePage.sideBarWidth = newWidth
                            }
                        }
                    }
                    Item { // mainChatContainer
                        id: mainChatContainer
                        property bool pendingScrollToBottom: false
                        property bool autoFollowBottom: false
                        property int scrollToBottomAttemptsRemaining: 0
                        // True when the open contact's key was revoked ("HACKED"): the
                        // message input is replaced with a notice and sending is disabled.
                        property bool currentContactRevoked: false
                        function refreshCurrentContactRevoked() {
                            currentContactRevoked = chats.currentIndex >= 0
                                ? contactsModel.getRevoked(chats.currentIndex)
                                : false
                        }
                        Connections {
                            target: chats
                            function onCurrentIndexChanged() {
                                mainChatContainer.refreshCurrentContactRevoked()
                            }
                        }
                        Connections {
                            target: MainSignals
                            function onIdentityRevocationReceived(serverId, contactPubKey) {
                                if (chats.currentIndex >= 0
                                    && serverId === contactsModel.getServer(chats.currentIndex)
                                    && contactPubKey === contactsModel.getPubKeyFingerprint(chats.currentIndex))
                                    mainChatContainer.currentContactRevoked = true
                            }
                        }
                        function performScrollToBottom() {
                            messagesList.forceLayout()
                            messagesList.positionViewAtEnd()
                        }
                        function followBottomAfterOpen() {
                            autoFollowBottom = true
                            scheduleScrollToBottom()
                        }
                        function stopFollowingBottom() {
                            autoFollowBottom = false
                            pendingScrollToBottom = false
                            scrollToBottomAttemptsRemaining = 0
                            scrollToBottomTimer.stop()
                        }
                        function followBottomIfNeeded() {
                            if (autoFollowBottom || pendingScrollToBottom)
                                scheduleScrollToBottom()
                        }
                        function tryScrollToBottom() {
                            if (!pendingScrollToBottom || !visible || messagesList.height <= 0 || messagesList.count <= 0)
                                return

                            performScrollToBottom()

                            if (messagesList.atYEnd) {
                                pendingScrollToBottom = false
                                scrollToBottomAttemptsRemaining = 0
                                scrollToBottomTimer.stop()
                                return
                            }

                            if (scrollToBottomAttemptsRemaining <= 0) {
                                pendingScrollToBottom = false
                                scrollToBottomTimer.stop()
                                performScrollToBottom()
                                return
                            }

                            scrollToBottomAttemptsRemaining -= 1
                            if (!scrollToBottomTimer.running)
                                scrollToBottomTimer.start()
                        }
                        function scheduleScrollToBottom() {
                            pendingScrollToBottom = true
                            if (scrollToBottomAttemptsRemaining <= 0)
                                scrollToBottomAttemptsRemaining = 48
                            tryScrollToBottom()
                        }
                        Timer {
                            id: scrollToBottomTimer
                            interval: 16
                            repeat: true
                            running: false
                            onTriggered: mainChatContainer.tryScrollToBottom()
                        }
                        clip: true
                        visible: chats.currentIndex !== -1 && (!mainPagePage.isMobileLayout || mainPagePage.mobilePane === mainPagePage.mobilePaneChat)
                        onVisibleChanged: {
                            if (visible)
                                tryScrollToBottom()
                            else
                                scrollToBottomTimer.stop()
                        }
                        onHeightChanged: tryScrollToBottom()
                        Layout.preferredWidth: mainPagePage.isMobileLayout ? parent.width : parent.width * 0.5
                        Layout.fillHeight: true
                        Layout.fillWidth: true
                        Layout.margins: mainPagePage.isMobileLayout ? 0 : 10
                        Layout.bottomMargin: 0
                        Layout.topMargin: 0
                        Layout.alignment: Qt.AlignRight

                        // Solid backdrop behind the floating panels (mobile only).
                        // Declared first so it paints behind everything; fills the
                        // margins around the header/composer pills with the normal
                        // chat background so they float on theme.background.
                        Rectangle {
                            id: chatBackdrop
                            anchors.fill: parent
                            visible: mainPagePage.isMobileLayout
                            color: theme.background
                        }

                        Rectangle {
                            id: chatHeaderRect
                            // On mobile the header becomes a rounded pill that floats
                            // with a margin on every side; on desktop it stays flush.
                            readonly property bool floating: mainPagePage.isMobileLayout
                            color: floating ? mainWindow.chatPanelSurface : theme.background
                            radius: floating ? height / 2 : 0
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.topMargin: floating ? 6 : 0
                            anchors.leftMargin: floating ? 8 : 0
                            anchors.rightMargin: floating ? 8 : 0
                            height: 56

                            PanelBorder {
                                visible: mainPagePage.isMobileLayout ? true : false
                                edgeColor: mainWindow.chatPanelBorderColor
                            }

                            Rectangle { // header avatar
                                id: headerAvatarContainer
                                width: mainPagePage.isMobileLayout ? 40 : 34
                                height: mainPagePage.isMobileLayout ? 40 : 34
                                anchors.left: parent.left
                                anchors.leftMargin: mainPagePage.isMobileLayout ? 50 : 10
                                anchors.verticalCenter: parent.verticalCenter
                                radius: width / 2
                                color: theme.minimumAccent
                                layer.enabled: true
                                Text {
                                    id: headerAvatarLetter
                                    anchors.centerIn: parent
                                    text: chats.currentIndex >= 0 && contactsModel.getFirstName(chats.currentIndex).length > 0
                                          ? contactsModel.getFirstName(chats.currentIndex)[0] : ""
                                    font.pixelSize: mainPagePage.isMobileLayout ? 18 : 16
                                    font.family: geologicaFont.name
                                    color: theme.accent
                                    visible: chats.currentIndex < 0 || !Account.hasContactAvatar(
                                        contactsModel.getServer(chats.currentIndex),
                                        contactsModel.getPubKeyFingerprint(chats.currentIndex)
                                    )
                                }
                                Image {
                                    id: headerAvatarImg
                                    anchors.fill: parent
                                    visible: false
                                    source: chats.currentIndex >= 0 ? Account.getContactAvatarSource(
                                        contactsModel.getServer(chats.currentIndex),
                                        contactsModel.getPubKeyFingerprint(chats.currentIndex)
                                    ) : ""
                                    mipmap: true
                                    fillMode: Image.PreserveAspectCrop
                                    smooth: true
                                    antialiasing: true
                                    layer.enabled: true
                                    layer.smooth: true
                                }
                                Connections {
                                    target: connections
                                    function onContactAvatarChanged(updatedServerId, updatedPubKeyFingerprint) {
                                        if (chats.currentIndex >= 0
                                                && updatedServerId === contactsModel.getServer(chats.currentIndex)
                                                && updatedPubKeyFingerprint === contactsModel.getPubKeyFingerprint(chats.currentIndex)) {
                                            headerAvatarImg.source = Account.getContactAvatarSource(
                                                contactsModel.getServer(chats.currentIndex),
                                                contactsModel.getPubKeyFingerprint(chats.currentIndex)
                                            ) + "?t=" + Date.now()
                                        }
                                    }
                                }
                                Connections {
                                    target: contactsModel
                                    function onContactUpdated(index) {
                                        if (index === chats.currentIndex) {
                                            headerAvatarLetter.text = contactsModel.getFirstName(chats.currentIndex).length > 0
                                                ? contactsModel.getFirstName(chats.currentIndex)[0] : ""
                                            headerAvatarLetter.visible = !Account.hasContactAvatar(
                                                contactsModel.getServer(chats.currentIndex),
                                                contactsModel.getPubKeyFingerprint(chats.currentIndex)
                                            )
                                            headerAvatarMaskEffect.visible = contactsModel.getAvatarSource(chats.currentIndex).length > 0
                                        }
                                    }
                                }
                                Rectangle { // mask
                                    id: headerAvatarMask
                                    anchors.fill: parent
                                    radius: headerAvatarContainer.radius
                                    visible: false
                                    layer.enabled: true
                                    layer.smooth: true
                                }
                                MultiEffect { // circle clip
                                    id: headerAvatarMaskEffect
                                    anchors.fill: parent
                                    source: headerAvatarImg
                                    maskEnabled: true
                                    maskSource: headerAvatarMask
                                    visible: chats.currentIndex >= 0 && contactsModel.getAvatarSource(chats.currentIndex).length > 0
                                    smooth: true
                                    antialiasing: true
                                    maskSpreadAtMin: 1.0
                                    maskThresholdMin: 0.6
                                }
                            } // header avatar
                            MouseArea {
                                id: headerInfoTapArea
                                visible: mainPagePage.isMobileLayout
                                anchors.left: parent.left
                                anchors.leftMargin: 50
                                anchors.right: parent.right
                                anchors.rightMargin: 126
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                onClicked: mainPagePage.showInfoPane()
                            }
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.leftMargin: mainPagePage.isMobileLayout ? 102 : 60
                                anchors.rightMargin: mainPagePage.isMobileLayout ? 126 : 10
                                anchors.topMargin: 8
                                anchors.bottomMargin: 10
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 0
                                StyledName { // contactNameText
                                    id: contactNameText
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    Layout.margins: 0
                                    Layout.rightMargin: 20
                                    Layout.preferredHeight: implicitHeight
                                    firstName: contactsModel.getFirstName(chats.currentIndex)
                                    lastName: contactsModel.getLastName(chats.currentIndex)
                                    styleCss: contactsModel.getNameStyle(chats.currentIndex)
                                    Connections { // !
                                        target: contactsModel
                                        function onContactUpdated(index) {
                                            if (index === chats.currentIndex) {
                                                contactNameText.firstName = contactsModel.getFirstName(chats.currentIndex)
                                                contactNameText.lastName = contactsModel.getLastName(chats.currentIndex)
                                                contactNameText.styleCss = contactsModel.getNameStyle(chats.currentIndex)
                                            }
                                        }
                                    }
                                    fallbackColor: theme.halfAccent
                                    pixelSize: 14
                                    fontFamily: "Roboto"
                                    elide: Text.ElideRight
                                }
                                Text { // lastOnlineText
                                    id: lastOnlineText
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    property string currentServerId: chats.currentIndex >= 0 ? contactsModel.getServer(chats.currentIndex) : ""
                                    property string currentContactPubKey: chats.currentIndex >= 0 ? contactsModel.getPubKeyFingerprint(chats.currentIndex) : ""
                                    property string currentLastOnlineText: ""
                                    property bool currentIsOnline: chats.currentIndex >= 0 ? contactsModel.getIsOnline(chats.currentIndex) : false
                                    property bool currentServerConnected: false

                                    function refreshStatus() {
                                        currentServerId = chats.currentIndex >= 0 ? contactsModel.getServer(chats.currentIndex) : ""
                                        currentContactPubKey = chats.currentIndex >= 0 ? contactsModel.getPubKeyFingerprint(chats.currentIndex) : ""
                                        currentIsOnline = chats.currentIndex >= 0 ? contactsModel.getIsOnline(chats.currentIndex) : false
                                        currentServerConnected = currentServerId.length > 0 ? connections.isConnected(currentServerId) : false

                                        if (!currentServerConnected && currentServerId.length > 0) {
                                            currentLastOnlineText = qsTr("No connection to server")
                                        } else if (chats.currentIndex >= 0) {
                                            currentLastOnlineText = contactsModel.getLastOnlineDescription(chats.currentIndex)
                                        } else {
                                            currentLastOnlineText = ""
                                        }
                                    }

                                    text: currentLastOnlineText
                                    Layout.margins: 0
                                    color: !currentServerConnected && currentServerId.length > 0 ? theme.redAccent : (currentIsOnline ? theme.accent : theme.lowAccent)
                                    opacity: 0.6
                                    font.pixelSize: 12
                                    font.family: geologicaFont.name
                                    elide: Text.ElideRight
                                    Component.onCompleted: refreshStatus()
                                }
                                Connections { // !
                                    target: contactsModel
                                    function onLastOnlineUpdated(index) {
                                        if (index === chats.currentIndex
                                                && lastOnlineText.currentServerId === contactsModel.getServer(index)
                                                && lastOnlineText.currentContactPubKey === contactsModel.getPubKeyFingerprint(index)) {
                                            lastOnlineText.refreshStatus()
                                        }
                                    }
                                    function onContactUpdated(index) {
                                        if (index === chats.currentIndex) {
                                            lastOnlineText.refreshStatus()
                                        }
                                    }
                                }
                                Connections {
                                    target: chats
                                    function onCurrentIndexChanged() {
                                        lastOnlineText.refreshStatus()
                                        onlineStatusIndicator.refreshStatus()
                                    }
                                }
                                Connections {
                                    target: connections
                                    function onConnectionStatusChanged(serverId, status) {
                                        if (serverId === lastOnlineText.currentServerId) {
                                            lastOnlineText.refreshStatus()
                                            onlineStatusIndicator.refreshStatus()
                                        }
                                    }
                                }
                            } // ColumnLayout
                            BottomButton {
                                id: mobileChatBackButton
                                visible: mainPagePage.isMobileLayout
                                height: parent.height
                                width: height
                                anchors.left: parent.left
                                anchors.leftMargin: 0
                                anchors.verticalCenter: parent.verticalCenter
                                icon.source: "resources/arrow-left-only.svg"
                                icon.color: theme.accent
                                icon.width: 23
                                icon.height: 23
                                onClicked: mainWindow.navigateBack()
                            }

                            BottomButton { // timerButton
                                id: timerButton
                                property string currentServerId: chats.currentIndex >= 0 ? contactsModel.getServer(chats.currentIndex) : ""
                                property string currentContactPubKey: chats.currentIndex >= 0 ? contactsModel.getPubKeyFingerprint(chats.currentIndex) : ""
                                property int currentTimerValue: 0

                                function refreshCurrentTimerValue() {
                                    if (chats.currentIndex < 0 || currentServerId.length === 0 || currentContactPubKey.length === 0) {
                                        currentTimerValue = 0
                                        return
                                    }

                                    currentTimerValue = contactsModel.getTimerValue(chats.currentIndex)
                                }

                                function setTimerValue(seconds) {
                                    // No-op when the picked duration already matches the active one,
                                    // so re-selecting the same value doesn't re-send/re-persist or
                                    // append a duplicate "Timer is set for..." info message.
                                    if (seconds === currentTimerValue) {
                                        return
                                    }
                                    MainSignals.emitSetTimer(currentServerId, currentContactPubKey, seconds)
                                }

                                height: parent.height
                                width: height
                                anchors.right: callButton.left
                                anchors.rightMargin: -14
                                anchors.leftMargin: 0
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.verticalCenterOffset: currentTimerValue !== 0 ? -5 : 0
                                icon.source: "resources/timer.svg"
                                icon.color: theme.accent
                                icon.width: 22
                                icon.height: 22
                                Component.onCompleted: refreshCurrentTimerValue()
                                onCurrentServerIdChanged: refreshCurrentTimerValue()
                                onCurrentContactPubKeyChanged: refreshCurrentTimerValue()
                                onClicked: { // !!!
                                    timerMenu.x = timerButton.x + timerButton.width - timerMenu.width
                                    timerMenu.y = timerButton.y + timerButton.height
                                    timerMenu.leftAlign = true
                                    timerMenu.isRed = false
                                    timerMenu.open()
                                }
                                PropertyAnimation {
                                    id: timerWobbleAnim1
                                    target: timerButton
                                    property: "rotation"
                                    from: 0
                                    to: 6
                                    duration: 60
                                    easing.type: Easing.InOutQuad
                                }
                                PropertyAnimation {
                                    id: timerWobbleAnim2
                                    target: timerButton
                                    property: "rotation"
                                    from: 6
                                    to: -6
                                    duration: 120
                                    easing.type: Easing.InOutQuad
                                }
                                PropertyAnimation {
                                    id: timerWobbleAnim3
                                    target: timerButton
                                    property: "rotation"
                                    from: -6
                                    to: 0
                                    duration: 60
                                    easing.type: Easing.InOutQuad
                                }
                                SequentialAnimation {
                                    id: timerWobbleAnim
                                    running: false
                                    loops: 2//Animation.Infinite
                                    onStopped: timerButton.rotation = 0
                                    animations: [
                                        timerWobbleAnim1,
                                        timerWobbleAnim2,
                                        timerWobbleAnim3
                                    ]
                                }
                                HoverHandler {
                                    id: timerHoverHandler
                                    onHoveredChanged: {
                                        if (hovered) {
                                            timerWobbleAnim.running = true
                                        } else {
                                            timerWobbleAnim.running = false
                                            timerButton.rotation = 0
                                        }
                                    }
                                }
                            } // timerButton
                            Rectangle {
                                id: timerValueIndicator
                                visible: timerButton.currentTimerValue !== 0
                                width: timerValueText.implicitWidth + 10
                                height: 20
                                radius: 8
                                color: theme.accent
                                border.color: theme.background
                                border.width: 2
                                anchors.horizontalCenter: timerButton.horizontalCenter
                                anchors.bottom: timerButton.bottom

                                Text {
                                    id: timerValueText
                                    anchors.centerIn: parent
                                    text: contactsModel.getTimerValueS(timerButton.currentTimerValue)
                                    color: theme.background
                                    font.pixelSize: 7
                                    font.family: "Roboto"
                                    font.weight: Font.Bold
                                }
                            }
                            Connections {
                                target: MainSignals
                                function handleSetTimer(serverId, contactPubKey, seconds) {
                                    if (serverId === timerButton.currentServerId
                                            && contactPubKey === timerButton.currentContactPubKey) {
                                        timerButton.currentTimerValue = seconds
                                    }
                                }
                                function onSetTimer(serverId, contactPubKey, seconds) {
                                    handleSetTimer(serverId, contactPubKey, seconds)
                                }
                                function onSetTimerReceived(serverId, contactPubKey, seconds) {
                                    handleSetTimer(serverId, contactPubKey, seconds)
                                }
                            }
                            Menu {
                                id: timerMenu
                                y: parent.height
                                topInset: 0
                                bottomInset: 0
                                padding: 0
                                property bool leftAlign: false
                                property bool isRed: false
                                background: Rectangle {
                                    implicitWidth: 150
                                    color: mainWindow.chatPanelSurface
                                    border.color: mainWindow.chatPanelBorderColor
                                    border.width: 1
                                    radius: 14

                                    PanelBorder {
                                        edgeColor: mainWindow.chatPanelBorderColor
                                    }
                                }
                                MyMenuItem {
                                    text: qsTr("Off")
                                    onTriggered: () => timerButton.setTimerValue(0)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                MyMenuItem {
                                    text: qsTr("1 second")
                                    onTriggered: () => timerButton.setTimerValue(1)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                MyMenuItem {
                                    text: qsTr("5 seconds")
                                    onTriggered: () => timerButton.setTimerValue(5)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                MyMenuItem {
                                    text: qsTr("10 seconds")
                                    onTriggered: () => timerButton.setTimerValue(10)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                MyMenuItem {
                                    text: qsTr("30 seconds")
                                    onTriggered: () => timerButton.setTimerValue(30)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                MyMenuItem {
                                    text: qsTr("1 minute")
                                    onTriggered: () => timerButton.setTimerValue(60)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                MyMenuItem {
                                    text: qsTr("5 minutes")
                                    onTriggered: () => timerButton.setTimerValue(300)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                MyMenuItem {
                                    text: qsTr("30 minutes")
                                    onTriggered: () => timerButton.setTimerValue(1800)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                MyMenuItem {
                                    text: qsTr("1 hour")
                                    onTriggered: () => timerButton.setTimerValue(3600)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                MyMenuItem {
                                    text: qsTr("12 hours")
                                    onTriggered: () => timerButton.setTimerValue(43200)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                MyMenuItem {
                                    text: qsTr("1 day")
                                    onTriggered: () => timerButton.setTimerValue(86400)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                MyMenuItem {
                                    text: qsTr("7 days")
                                    onTriggered: () => timerButton.setTimerValue(604800)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: ""
                                }
                                
                            }
                            BottomButton { // callButton
                                id: callButton
                                height: parent.height
                                width: height
                                anchors.right: Platform.isMobile ? chatMenuButton.left : parent.right
                                anchors.rightMargin: Platform.isMobile ? -17 : 0
                                anchors.leftMargin: 0
                                anchors.verticalCenter: parent.verticalCenter
                                icon.source: "resources/call.svg"
                                icon.color: theme.accent
                                icon.width: 20
                                icon.height: 20
                                onClicked: { // !!!
                                    console.log("Call button clicked")
                                    // The call always goes through: the call_request is sent
                                    // even if the contact is offline. The server queues it and
                                    // delivers it once the contact comes back online.
                                    callManager.startCall(
                                        contactsModel.getFirstName(chats.currentIndex),
                                        contactsModel.getAvatarSource(chats.currentIndex),
                                        contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                        contactsModel.getServer(chats.currentIndex)
                                    );
                                }
                                PropertyAnimation {
                                    id: callWobbleAnim1
                                    target: callButton
                                    property: "rotation"
                                    from: 0
                                    to: 6
                                    duration: 60
                                    easing.type: Easing.InOutQuad
                                }
                                PropertyAnimation {
                                    id: callWobbleAnim2
                                    target: callButton
                                    property: "rotation"
                                    from: 6
                                    to: -6
                                    duration: 120
                                    easing.type: Easing.InOutQuad
                                }
                                PropertyAnimation {
                                    id: callWobbleAnim3
                                    target: callButton
                                    property: "rotation"
                                    from: -6
                                    to: 0
                                    duration: 60
                                    easing.type: Easing.InOutQuad
                                }
                                SequentialAnimation {
                                    id: callWobbleAnim
                                    running: false
                                    loops: 2//Animation.Infinite
                                    onStopped: callButton.rotation = 0
                                    animations: [
                                        callWobbleAnim1,
                                        callWobbleAnim2,
                                        callWobbleAnim3
                                    ]
                                }
                                HoverHandler {
                                    id: callHoverHandler
                                    onHoveredChanged: {
                                        if (hovered) {
                                            callWobbleAnim.running = true
                                        } else {
                                            callWobbleAnim.running = false
                                            callButton.rotation = 0
                                        }
                                    }
                                }
                                Connections { // !!!
                                    target: connections
                                    function onCallRequestReceived(contactFirstName, contactAvatar, contactServer, contactPubKey, dh_pub, id, callId) {
                                        callManager.showCallRequest(contactFirstName, contactAvatar, contactServer, contactPubKey, dh_pub, id, callId);
                                    }
                                }
                            } // chatMenuButton
                            BottomButton {
                                id: chatMenuButton
                                visible: mainPagePage.isMobileLayout
                                height: parent.height
                                width: height
                                anchors.right: parent.right
                                anchors.rightMargin: 5
                                anchors.leftMargin: 0
                                anchors.verticalCenter: parent.verticalCenter
                                icon.source: "resources/menu.svg"
                                icon.color: theme.accent
                                icon.width: 25
                                icon.height: 25
                                onClicked: {
                                    chatHeaderMenu.x = Math.max(0, chatMenuButton.x + chatMenuButton.width - chatHeaderMenu.width)
                                    chatHeaderMenu.y = chatMenuButton.y + chatMenuButton.height
                                    chatHeaderMenu.open()
                                }
                            }
                            Menu {
                                id: chatHeaderMenu
                                topInset: 0
                                bottomInset: 0
                                padding: 0
                                background: Rectangle {
                                    // Grow with the (content-sized) item so long
                                    // translations like RU "Информация о контакте" fit.
                                    implicitWidth: Math.max(180, contactInfoItem.implicitWidth)
                                    color: mainWindow.chatPanelSurface
                                    border.color: mainWindow.chatPanelBorderColor
                                    border.width: 1
                                    radius: 14

                                    PanelBorder {
                                        edgeColor: mainWindow.chatPanelBorderColor
                                    }
                                }
                                MyMenuItem {
                                    id: contactInfoItem
                                    text: qsTr("Contact info")
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    onTriggered: mainPagePage.showInfoPane()
                                    iconSource: "resources/info.svg"
                                }
                            }
                        }
                        Rectangle { // messagesContainer
                            id: messagesContainer
                            anchors.top: chatHeaderRect.bottom
                            anchors.bottom: topBorder.top
                            anchors.left: parent.left
                            anchors.right: parent.right
                            color: theme.background
                            clip: true
                            Menu {
                                id: sharedMessageMenu
                                y: parent.height
                                topInset: 0
                                bottomInset: 0
                                padding: 0
                                property bool leftAlign: true
                                property var messageId: 0
                                property string messageText: ""
                                property bool messageIsOwn: false
                                property string messageKind: ""
                                property string messagePreview: ""
                                property bool messageReplyable: true
                                property bool messageDownloadable: false
                                background: Rectangle {
                                    implicitWidth: 150
                                    color: mainWindow.chatPanelSurface
                                    border.color: mainWindow.chatPanelBorderColor
                                    border.width: 1
                                    radius: 14

                                    PanelBorder {
                                        edgeColor: mainWindow.chatPanelBorderColor
                                    }
                                }
                                MyMenuItem {
                                    text: qsTr("Reply")
                                    visible: sharedMessageMenu.messageReplyable
                                    height: visible ? implicitHeight : 0
                                    onTriggered: replyDraft.start(
                                        sharedMessageMenu.messageId,
                                        sharedMessageMenu.messageIsOwn,
                                        sharedMessageMenu.messageKind,
                                        sharedMessageMenu.messagePreview)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: "resources/reply.svg"
                                }
                                MyMenuItem {
                                    text: qsTr("Download")
                                    visible: sharedMessageMenu.messageDownloadable
                                    height: visible ? implicitHeight : 0
                                    onTriggered: fileTransferManager.downloadToDownloads(
                                        contactsModel.getServer(chats.currentIndex),
                                        contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                        sharedMessageMenu.messageId)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: "resources/download.svg"
                                }
                                MyMenuItem {
                                    text: qsTr("Copy")
                                    onTriggered: ClipboardHelper.setText(sharedMessageMenu.messageText)
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: "resources/copy.svg"
                                }
                                MyMenuItem {
                                    text: qsTr("Delete")
                                    isRed: true
                                    onTriggered: MainSignals.emitMessageDelete(sharedMessageMenu.messageId, contactsModel.getServer(chats.currentIndex), contactsModel.getPubKeyFingerprint(chats.currentIndex))
                                    leftAlign: true
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: "resources/remove.svg"
                                }
                            }
                            ListView { // messagesList
                                id: messagesList
                                anchors.fill: parent
                                anchors.topMargin: 10
                                anchors.leftMargin: mainPagePage.isMobileLayout ? 12 : 0
                                anchors.rightMargin: mainPagePage.isMobileLayout ? 12 : 0
                                spacing: 0
                                model: messageModel
                                reuseItems: false
                                // mid of the message to briefly highlight (after jumping to
                                // it from a reply quote); 0 = none. Cleared by the timer.
                                property double highlightMid: 0
                                Timer {
                                    id: replyHighlightTimer
                                    interval: 550
                                    onTriggered: messagesList.highlightMid = 0
                                }
                                // Re-centers a reply-jump target a few times: when the message is
                                // far off-screen its delegate (and neighbours) aren't measured yet,
                                // so the first positionViewAtIndex lands on estimated positions.
                                // Each re-position realizes/measures more rows and converges.
                                property int replyJumpIndex: -1
                                Timer {
                                    id: replyJumpTimer
                                    interval: 40
                                    repeat: true
                                    property int shots: 0
                                    onTriggered: {
                                        if (messagesList.replyJumpIndex < 0) { stop(); shots = 0; return }
                                        messagesList.forceLayout()
                                        messagesList.positionViewAtIndex(messagesList.replyJumpIndex, ListView.Center)
                                        shots += 1
                                        if (shots >= 4) { stop(); shots = 0; messagesList.replyJumpIndex = -1 }
                                    }
                                }
                                // Jump to (and briefly pulse) the message with the given mid.
                                // Lives on the ListView so both a reply-quote bubble AND the
                                // compose reply strip above the input can drive it.
                                function jumpToRepliedMid(mid) {
                                    if (!mid || mid <= 0)
                                        return
                                    var idx = messageModel.indexOfMessageId(mid)
                                    if (idx < 0)
                                        return
                                    // Stop stick-to-bottom first, otherwise the delegates that
                                    // realize during the jump grow contentHeight and snap the
                                    // view back to the end — the "flicker" the user saw.
                                    mainChatContainer.stopFollowingBottom()
                                    messagesList.forceLayout()
                                    messagesList.positionViewAtIndex(idx, ListView.Center)
                                    // Correct the landing over the next few frames (far items).
                                    messagesList.replyJumpIndex = idx
                                    replyJumpTimer.restart()
                                    // Re-arm cleanly so re-tapping the same quote pulses again.
                                    messagesList.highlightMid = 0
                                    messagesList.highlightMid = mid
                                    replyHighlightTimer.restart()
                                }
                                // // Smooth scrolling // ?
                                // boundsBehavior: Flickable.StopAtBounds
                                onContentHeightChanged: mainChatContainer.followBottomIfNeeded()
                                onContentYChanged: {
                                    if ((dragging || flicking) && !atYEnd)
                                        mainChatContainer.stopFollowingBottom()
                                }
                                onMovementEnded: mainChatContainer.autoFollowBottom = atYEnd
                                onDraggingChanged: {
                                    if (dragging && !atYEnd)
                                        mainChatContainer.stopFollowingBottom()
                                    else if (!dragging)
                                        mainChatContainer.autoFollowBottom = atYEnd
                                }
                                WheelScroller {
                                    flickable: messagesList
                                    // Wheel moves contentY directly, so the drag/flick
                                    // handlers above never run: keep the stick-to-bottom
                                    // flag in sync here, same as onMovementEnded.
                                    onScrolled: mainChatContainer.autoFollowBottom = messagesList.atYEnd
                                }
                                Connections { // !
                                    target: messageModel
                                    function onChatToBottom() {
                                        mainChatContainer.followBottomAfterOpen()
                                    }
                                }
                                delegate: Item {
                                    id: delegateItem
                                    width: messagesList.width
                                    height: bubble.height + 10
                                    property var messageId: model.mid
                                    readonly property bool isInViewport: (y + height) >= (messagesList.contentY - 96)
                                        && y <= (messagesList.contentY + messagesList.height + 96)
                                    readonly property bool isMissedCall: isCall
                                        && (model.text === "Missed Call" || model.text === "Contact is busy")
                                    function fileTypeIcon(name) {
                                        var ext = name.toLowerCase().split('.').pop()
                                        switch (ext) {
                                        case "mp3": case "flac": case "wav": case "ogg":
                                        case "aac": case "m4a": case "wma": case "opus":
                                            return "resources/music.svg"
                                        case "jpg": case "jpeg": case "png": case "bmp":
                                        case "gif": case "webp": case "tiff":
                                            return "resources/image.svg"
                                        case "mp4": case "avi": case "mkv": case "mov":
                                        case "flv": case "webm": case "m4v": case "wmv":
                                            return "resources/video.svg"
                                        case "db": case "sqlite": case "sqlite3": case "sql":
                                            return "resources/db.svg"
                                        case "txt":
                                            return "resources/txt.svg"
                                        case "xlsx": case "xls":
                                            return "resources/xlsx.svg"
                                        case "docx": case "doc":
                                            return "resources/docx.svg"
                                        case "pdf":
                                            return "resources/pdf.svg"
                                        case "apk":
                                            return "resources/apk.svg"
                                        case "c": case "cpp": case "cc": case "cxx": case "h": case "hpp":
                                        case "py": case "pyx": case "pyd":
                                        case "go":
                                        case "cs":
                                        case "pas": case "pp":
                                        case "bas": case "vb":
                                        case "rs":
                                        case "php":
                                        case "js": case "jsx": case "mjs": case "cjs":
                                        case "ts": case "tsx":
                                        case "rb":
                                        case "java": case "kt": case "kts":
                                        case "swift":
                                        case "lua":
                                        case "r":
                                        case "m": case "mm":
                                        case "scala": case "sc":
                                        case "sh": case "bash": case "zsh": case "fish":
                                        case "pl": case "pm":
                                        case "hs": case "lhs":
                                        case "ex": case "exs":
                                        case "dart":
                                        case "zig":
                                        case "nim":
                                        case "d":
                                        case "jl":
                                        case "clj": case "cljs":
                                        case "ml": case "mli":
                                        case "f": case "f90": case "f95":
                                        case "asm": case "s":
                                            return "resources/code.svg"
                                        default:
                                            return "resources/file.svg"
                                        }
                                    }
                                    // Horizontal offset while swiping-to-reply (mobile). Snaps
                                    // back to 0 on release; the Behavior is disabled mid-drag so
                                    // the bubble tracks the finger 1:1.
                                    property real swipeOffset: 0
                                    Behavior on swipeOffset {
                                        enabled: !swipeHandler.active
                                        NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                                    }
                                    function scrollToReplied(mid) {
                                        messagesList.jumpToRepliedMid(mid)
                                    }
                                    function startReplyFromModel() {
                                        if (isDate || isInfo)
                                            return
                                        replyDraft.start(model.mid, isOwn,
                                            mainWindow.replyKindFor(model),
                                            mainWindow.replyPreviewFor(model))
                                    }
                                    function openMessageMenu(menuX, menuY) {
                                        if (isDate)
                                            return

                                        const menuPosition = delegateItem.mapToItem(messagesContainer, menuX, menuY)
                                        sharedMessageMenu.x = Math.max(0, menuPosition.x)
                                        sharedMessageMenu.y = Math.max(0, menuPosition.y)
                                        sharedMessageMenu.leftAlign = !isOwn
                                        sharedMessageMenu.messageId = messageId
                                        sharedMessageMenu.messageText = mainWindow.callStatusText(model.text)
                                        sharedMessageMenu.messageIsOwn = isOwn
                                        sharedMessageMenu.messageKind = mainWindow.replyKindFor(model)
                                        sharedMessageMenu.messagePreview = mainWindow.replyPreviewFor(model)
                                        sharedMessageMenu.messageReplyable = !isInfo
                                        sharedMessageMenu.messageDownloadable = isFile || isImage || isVideo || isAudio || isAlbum
                                        sharedMessageMenu.open()
                                    }
                                    MouseArea { // messageMouseArea
                                        anchors.fill: parent
                                        acceptedButtons: Qt.RightButton | Qt.LeftButton
                                        onClicked: (mouse) => {
                                            if (mouse.button === Qt.RightButton)
                                                delegateItem.openMessageMenu(mouse.x, mouse.y)
                                        }
                                        onPressAndHold: function(mouse) {
                                            if (!mainPagePage.isMobileLayout)
                                                return

                                            const fallbackX = isOwn
                                                ? delegateItem.width - 162
                                                : 12
                                            const fallbackY = bubble.y + (bubble.height / 2)
                                            delegateItem.openMessageMenu(
                                                mouse ? mouse.x : fallbackX,
                                                mouse ? mouse.y : fallbackY
                                            )
                                        }
                                    }
                                    // Swipe-left-to-reply (mobile). xAxis-only so vertical scrolling
                                    // still goes to the ListView; only a leftward drag reveals the
                                    // reply affordance and, past the threshold, starts the reply.
                                    DragHandler {
                                        id: swipeHandler
                                        enabled: mainPagePage.isMobileLayout && !isDate && !isInfo
                                        target: null
                                        xAxis.enabled: true
                                        yAxis.enabled: false
                                        onActiveTranslationChanged: {
                                            if (active)
                                                delegateItem.swipeOffset = Math.max(-90, Math.min(0, activeTranslation.x))
                                        }
                                        onActiveChanged: {
                                            if (!active) {
                                                if (delegateItem.swipeOffset <= -46)
                                                    delegateItem.startReplyFromModel()
                                                delegateItem.swipeOffset = 0
                                            }
                                        }
                                    }
                                    ColorImage {
                                        source: "resources/reply.svg"
                                        accentColor: theme.accent
                                        width: 22
                                        height: 22
                                        anchors.right: parent.right
                                        anchors.rightMargin: 14
                                        anchors.verticalCenter: bubble.verticalCenter
                                        visible: mainPagePage.isMobileLayout && delegateItem.swipeOffset < 0
                                        opacity: Math.min(1, Math.abs(delegateItem.swipeOffset) / 46)
                                    }
                                    Rectangle {
                                        id: bubble
                                        transform: Translate { x: delegateItem.swipeOffset }
                                        // Brief brightness pulse when jumped-to from a reply quote.
                                        // Driven by an explicit animation (not a State/Transition,
                                        // which wouldn't fade in for a just-created off-screen
                                        // delegate) triggered when highlightMid targets this message.
                                        Rectangle {
                                            id: highlightOverlay
                                            anchors.fill: parent
                                            radius: bubble.radius
                                            z: 100
                                            color: "white"
                                            opacity: 0
                                            visible: opacity > 0
                                            SequentialAnimation {
                                                id: highlightAnim
                                                NumberAnimation { target: highlightOverlay; property: "opacity"; from: 0.0; to: 0.4; duration: 160 }
                                                PauseAnimation { duration: 140 }
                                                NumberAnimation { target: highlightOverlay; property: "opacity"; to: 0.0; duration: 340 }
                                            }
                                            Connections {
                                                target: messagesList
                                                function onHighlightMidChanged() {
                                                    if (messagesList.highlightMid !== 0
                                                            && Number(messagesList.highlightMid) === Number(delegateItem.messageId))
                                                        highlightAnim.restart()
                                                }
                                            }
                                        }
                                        readonly property bool mediaMessage: isFile && !isAudio && (isImage || isVideo)
                                        readonly property real maxPossibleWidth: messagesList.width * 0.75
                                        readonly property real leftMargin: 0
                                        readonly property real rightMargin: 0
                                        readonly property real internalPadding: leftMargin + rightMargin
                                        readonly property real preferredFileWidth: Math.max(196, Math.min(maxPossibleWidth, 296))
                                        readonly property real preferredAudioWidth: Math.max(220, Math.min(maxPossibleWidth, 320))
                                        readonly property real preferredMediaWidth: Math.max(240, Math.min(maxPossibleWidth, 360))
                                        readonly property real preferredAlbumWidth: Math.max(260, Math.min(maxPossibleWidth, 380))
                                        // The bubble must be wide enough for the reply quote (if any).
                                        readonly property bool hasReply: model.replyTo && model.replyTo > 0
                                        readonly property real replyMinWidth: hasReply
                                            ? Math.min(maxPossibleWidth,
                                                Math.max(replyAuthorMetrics.boundingRect.width, replyPreviewMetrics.boundingRect.width) + 34)
                                            : 0
                                        // Fonts.richTextWidth (not a plain TextMetrics) — the body
                                        // renders via Fonts.richText() with emoji runs at 1.3x size
                                        // through a different font entirely; measuring the raw
                                        // string through a single plain font underestimates it
                                        // badly (Roboto has no emoji glyphs at all), leaving the
                                        // bubble too narrow for its own content and wrapping at
                                        // seemingly-arbitrary points clustered around the emoji.
                                        readonly property real fullTextWidth: Math.max(
                                            Fonts.richTextWidth(model.text, 14) + internalPadding + 20,
                                            isAudio ? preferredAudioWidth : ((isImage || isVideo) ? preferredMediaWidth : (isFile ? preferredFileWidth : 0))
                                        )
                                        readonly property real callArrowIconSize: 16
                                        readonly property real callSecondColumnIconSize: 18
                                        readonly property real callRowSpacing: 24
                                        readonly property real callIconTimeSpacing: 6
                                        readonly property real callIconTrailingMargin: 16
                                        readonly property real callContentSideMargins: 18
                                        readonly property real callBubbleWidth: internalPadding
                                            + Math.max(callTitleMetrics.boundingRect.width, callArrowIconSize + callIconTimeSpacing + callTimeMetrics.boundingRect.width)
                                            + callRowSpacing + callSecondColumnIconSize + callIconTrailingMargin + callContentSideMargins
                                        width: isInfo ? (dateText.width + internalPadding) :
                                            (isDate ? (dateText.implicitWidth + internalPadding) :
                                            (isCall ? Math.max(80, Math.min(callBubbleWidth, maxPossibleWidth)) :
                                            Math.max(replyMinWidth,
                                            (isAlbum ? preferredAlbumWidth : (isAudio ? preferredAudioWidth : ((isImage || isVideo) ? preferredMediaWidth : (isFile ? preferredFileWidth : Math.max(80, Math.min(fullTextWidth, maxPossibleWidth)))))))))
                                        height: (isDate || isInfo) ? (dateText.implicitHeight + 12) : (contentColumn.height + 16)
                                        radius: 14
                                        // Base fill of a real (non-transparent) bubble, kept as its own
                                        // property so the rim below derives from the exact same color.
                                        readonly property color bubbleFill: isOwn ? theme.aroundZeroAccent : theme.aroundZeroAccent
                                        // Rim colour = the bubble's *visible* colour (fill flattened over
                                        // the theme background) lifted 2% toward white — a hairline a touch
                                        // brighter than the bubble it traces.
                                        readonly property color bubbleRimColor: mainWindow.mixColors(mainWindow.compositeOver(bubbleFill, theme.background), Qt.rgba(1, 1, 1, 1), 0.01)
                                        color: (isDate || isInfo || mediaMessage) ? "transparent" : bubbleFill
                                        // Rim tracing the full rounded silhouette (see PanelBorder.qml),
                                        // only on real filled bubbles — date/info separators and the
                                        // transparent single-media message (square-cropped image) get none.
                                        PanelBorder {
                                            visible: !(isDate || isInfo || bubble.mediaMessage)
                                            cornerRadius: bubble.radius
                                            edgeColor: bubble.bubbleRimColor
                                        }
                                        anchors.horizontalCenter: (isDate || isInfo) ? parent.horizontalCenter : undefined
                                        anchors.right: (!(isDate || isInfo) && isOwn) ? parent.right : undefined
                                        anchors.left: (!(isDate || isInfo) && !isOwn) ? parent.left : undefined
                                        anchors.rightMargin: rightMargin
                                        anchors.leftMargin: leftMargin
                                        TextMetrics {
                                            id: replyAuthorMetrics
                                            text: bubble.hasReply ? mainWindow.replyAuthorName(model.replyIsOwn, contactsModel.getFirstName(chats.currentIndex), contactsModel.getLastName(chats.currentIndex)) : ""
                                            font.pixelSize: 12
                                            font.family: geologicaFont.name
                                            font.weight: Font.DemiBold
                                        }
                                        TextMetrics {
                                            id: replyPreviewMetrics
                                            text: bubble.hasReply ? mainWindow.replyLabel(model.replyKind, model.replyPreview) : ""
                                            font.pixelSize: 12
                                            font.family: geologicaFont.name
                                        }
                                        TextMetrics {
                                            id: callTitleMetrics
                                            text: isMissedCall ? qsTr("Missed Call") : (isOwn ? qsTr("Outgoing Call") : qsTr("Incoming Call"))
                                            font.pixelSize: 13
                                            font.family: geologicaFont.name
                                            font.weight: Font.Normal
                                        }
                                        TextMetrics {
                                            id: callTimeMetrics
                                            text: model.callDurationSec > 0
                                                ? (model.time + " · " + mainWindow.formatDuration(model.callDurationSec * 1000))
                                                : model.time
                                            font.pixelSize: 10
                                        }
                                        Text { // dateText
                                            id: dateText
                                            visible: isDate || isInfo
                                            text: model.text
                                            anchors.centerIn: parent
                                            font.pixelSize: 12
                                            color: theme.halfAccent
                                            anchors.rightMargin: 2
                                            opacity: isInfo ? 0.8 : 1.0
                                            // isDate strings are always short (e.g. "Today"), so let them
                                            // hug their natural width. isInfo strings are full sentences
                                            // (server/transfer errors) that must wrap instead of pushing the
                                            // bubble past the screen edge.
                                            wrapMode: isInfo ? Text.WordWrap : Text.NoWrap
                                            horizontalAlignment: Text.AlignHCenter
                                            width: isInfo ? Math.min(implicitWidth, bubble.maxPossibleWidth - bubble.internalPadding) : implicitWidth
                                        }
                                        Column {
                                            id: contentColumn
                                            visible: !(isDate || isInfo)
                                            anchors.left: parent.left
                                            anchors.top: parent.top
                                            anchors.margins: 8
                                            anchors.leftMargin: 10
                                            spacing: 4
                                            width: bubble.width - 18
                                            // Reply quote: a one-line snapshot of the message this
                                            // one replies to, shown above the content (text/file/media).
                                            Rectangle {
                                                id: replyQuote
                                                visible: bubble.hasReply
                                                width: parent.width
                                                height: visible ? (replyQuoteCol.implicitHeight + 10) : 0
                                                radius: 6
                                                color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.12)
                                                Rectangle {
                                                    id: replyQuoteBar
                                                    width: 3
                                                    radius: 1.5
                                                    anchors.left: parent.left
                                                    anchors.top: parent.top
                                                    anchors.bottom: parent.bottom
                                                    anchors.margins: 5
                                                    color: theme.accent
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: delegateItem.scrollToReplied(model.replyTo)
                                                }
                                                Column {
                                                    id: replyQuoteCol
                                                    anchors.left: replyQuoteBar.right
                                                    anchors.leftMargin: 8
                                                    anchors.right: parent.right
                                                    anchors.rightMargin: 8
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    spacing: 1
                                                    Text {
                                                        text: mainWindow.replyAuthorName(model.replyIsOwn, contactsModel.getFirstName(chats.currentIndex), contactsModel.getLastName(chats.currentIndex))
                                                        color: theme.accent
                                                        font.family: geologicaFont.name
                                                        font.pixelSize: 12
                                                        font.weight: Font.DemiBold
                                                        elide: Text.ElideRight
                                                        width: parent.width
                                                    }
                                                    Text {
                                                        text: mainWindow.replyLabel(model.replyKind, model.replyPreview)
                                                        color: theme.accent
                                                        opacity: 0.85
                                                        font.family: "Roboto"
                                                        font.pixelSize: 12
                                                        elide: Text.ElideRight
                                                        maximumLineCount: 1
                                                        width: parent.width
                                                    }
                                                }
                                            }
                                            Loader {
                                                id: messageContentLoader
                                                active: contentColumn.visible
                                                visible: active
                                                width: parent.width
                                                sourceComponent: isCall
                                                    ? callContentComponent
                                                    : (isAlbum
                                                       ? albumContentComponent
                                                       : (isAudio
                                                       ? audioContentComponent
                                                       : ((isFile && !isAudio && (isImage || isVideo))
                                                          ? mediaContentComponent
                                                          : ((isFile && !isAudio && !isImage && !isVideo)
                                                             ? fileContentComponent
                                                             : textContentComponent))))
                                                height: item ? item.implicitHeight : 0
                                            }
                                            Component {
                                                id: albumContentComponent

                                                Column {
                                                    id: albumRoot
                                                    width: contentColumn.width
                                                    spacing: 6
                                                    readonly property var allItems: model.albumItems ? model.albumItems : []
                                                    readonly property var mediaItems: allItems.filter(function(it) { return it.isImage || it.isVideo })
                                                    readonly property var fileItems: allItems.filter(function(it) { return !it.isImage && !it.isVideo })
                                                    // Transient per-item download state, kept separate from the item list
                                                    // above so progress ticks don't rebuild the grid/list delegates.
                                                    readonly property var progressList: model.albumProgress ? model.albumProgress : []
                                                    function itemDownloading(idx) {
                                                        return (progressList && idx < progressList.length && progressList[idx]) ? progressList[idx].downloading : false
                                                    }

                                                    function albumServer() { return contactsModel.getServer(chats.currentIndex) }
                                                    function albumPubKey() { return contactsModel.getPubKeyFingerprint(chats.currentIndex) }
                                                    function activateItem(it) {
                                                        if (it.downloaded) {
                                                            // Open a swipeable viewer over every downloaded photo/video
                                                            // in this album, starting on the tapped item.
                                                            var media = albumRoot.mediaItems
                                                            var list = []
                                                            var start = 0
                                                            for (var i = 0; i < media.length; ++i) {
                                                                var m = media[i]
                                                                if (!m.downloaded)
                                                                    continue
                                                                if (m.index === it.index)
                                                                    start = list.length
                                                                list.push({ path: m.localPath, isVideo: m.isVideo, title: m.fileName })
                                                            }
                                                            if (list.length === 0)
                                                                mainWindow.openMediaViewer(it.localPath, it.isVideo, it.fileName)
                                                            else
                                                                mainWindow.openMediaViewerList(list, start)
                                                        } else if (!itemDownloading(it.index)) {
                                                            fileTransferManager.downloadAlbumItem(albumServer(), albumPubKey(), model.mid, it.index)
                                                        }
                                                    }

                                                    // ---- Media grid ----
                                                    Item {
                                                        id: mediaGrid
                                                        width: parent.width
                                                        visible: albumRoot.mediaItems.length > 0
                                                        readonly property var items: albumRoot.mediaItems
                                                        readonly property int n: items.length
                                                        readonly property real g: 3
                                                        readonly property real halfW: (width - g) / 2
                                                        readonly property real baseH: Math.min(width, 300)
                                                        function cols() { return n >= 5 ? 3 : 2 }
                                                        function cell5() { return (width - 2 * g) / 3 }
                                                        height: n <= 0 ? 0
                                                            : (n === 1 ? width * 0.66
                                                            : (n === 2 ? halfW
                                                            : (n === 3 ? baseH
                                                            : (n === 4 ? width
                                                            : (Math.ceil(n / 3) * cell5() + (Math.ceil(n / 3) - 1) * g)))))

                                                        function cellX(i) {
                                                            if (n === 1) return 0
                                                            if (n === 2) return i === 0 ? 0 : halfW + g
                                                            if (n === 3) return i === 0 ? 0 : halfW + g
                                                            if (n === 4) return (i % 2 === 0) ? 0 : halfW + g
                                                            return (i % 3) * (cell5() + g)
                                                        }
                                                        function cellY(i) {
                                                            if (n <= 2) return 0
                                                            if (n === 3) return i === 2 ? (baseH - g) / 2 + g : 0
                                                            if (n === 4) return i < 2 ? 0 : halfW + g
                                                            return Math.floor(i / 3) * (cell5() + g)
                                                        }
                                                        function cellW(i) {
                                                            if (n === 1) return width
                                                            if (n >= 5) return cell5()
                                                            return halfW
                                                        }
                                                        function cellH(i) {
                                                            if (n === 1) return width * 0.66
                                                            if (n === 2) return halfW
                                                            if (n === 3) return i === 0 ? baseH : (baseH - g) / 2
                                                            if (n === 4) return halfW
                                                            return cell5()
                                                        }

                                                        Repeater {
                                                            model: mediaGrid.items
                                                            delegate: Rectangle {
                                                                x: mediaGrid.cellX(index)
                                                                y: mediaGrid.cellY(index)
                                                                width: mediaGrid.cellW(index)
                                                                height: mediaGrid.cellH(index)
                                                                radius: 6
                                                                clip: true
                                                                // Media placeholder tint — lifted above both (now dark)
                                                                // bubble backgrounds so the tile reads before its image loads.
                                                                color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.08)

                                                                Image {
                                                                    anchors.fill: parent
                                                                    visible: modelData.isImage && modelData.downloaded
                                                                    source: (modelData.isImage && modelData.downloaded) ? mainWindow.toLocalFileUrl(modelData.localPath) : ""
                                                                    fillMode: Image.PreserveAspectCrop
                                                                    asynchronous: true
                                                                    cache: false
                                                                }

                                                                // Decoded video frame for album items — there is no separate
                                                                // thumbnail concept in this app, so mirror the single-video-message
                                                                // preview (mediaContentComponent's videoPreviewLoader below).
                                                                Loader {
                                                                    id: albumVideoPreviewLoader
                                                                    anchors.fill: parent
                                                                    active: modelData.isVideo && modelData.downloaded && delegateItem.isInViewport
                                                                    visible: active

                                                                    sourceComponent: Item {
                                                                        anchors.fill: parent

                                                                        AudioOutput {
                                                                            id: albumPreviewAudioOutput
                                                                            volume: 0.0
                                                                            muted: true
                                                                        }

                                                                        MediaPlayer {
                                                                            id: albumPreviewMediaPlayer
                                                                            source: mainWindow.toLocalFileUrl(modelData.localPath)
                                                                            audioOutput: albumPreviewAudioOutput
                                                                            videoOutput: albumPreviewVideoOutput
                                                                            loops: MediaPlayer.Infinite
                                                                            readonly property int staticPreviewPausePositionMs: 1
                                                                            property bool staticPreviewPauseApplied: false

                                                                            function refreshPreviewPlayback() {
                                                                                staticPreviewPauseApplied = false

                                                                                if (mainWindow.liveMediaPreviewEnabled) {
                                                                                    if (playbackState !== MediaPlayer.PlayingState)
                                                                                        play()
                                                                                    return
                                                                                }

                                                                                position = 0
                                                                                if (playbackState !== MediaPlayer.PlayingState)
                                                                                    play()
                                                                            }

                                                                            Component.onCompleted: refreshPreviewPlayback()
                                                                            Component.onDestruction: stop()

                                                                            onPositionChanged: {
                                                                                if (!mainWindow.liveMediaPreviewEnabled
                                                                                        && !staticPreviewPauseApplied
                                                                                        && playbackState === MediaPlayer.PlayingState
                                                                                        && position >= staticPreviewPausePositionMs) {
                                                                                    staticPreviewPauseApplied = true
                                                                                    pause()
                                                                                }
                                                                            }

                                                                            onMediaStatusChanged: {
                                                                                if (mediaStatus === MediaPlayer.LoadedMedia || mediaStatus === MediaPlayer.BufferedMedia) {
                                                                                    if (mainWindow.liveMediaPreviewEnabled) {
                                                                                        if (playbackState !== MediaPlayer.PlayingState)
                                                                                            play()
                                                                                    } else if (!staticPreviewPauseApplied && playbackState === MediaPlayer.StoppedState) {
                                                                                        play()
                                                                                    }
                                                                                }
                                                                            }
                                                                        }

                                                                        Connections {
                                                                            target: mainWindow
                                                                            function onLiveMediaPreviewEnabledChanged() {
                                                                                albumPreviewMediaPlayer.refreshPreviewPlayback()
                                                                            }
                                                                        }

                                                                        VideoOutput {
                                                                            id: albumPreviewVideoOutput
                                                                            anchors.fill: parent
                                                                            fillMode: VideoOutput.PreserveAspectCrop
                                                                        }
                                                                    }
                                                                }

                                                                // Play badge for video (downloaded or not)
                                                                Rectangle {
                                                                    anchors.centerIn: parent
                                                                    visible: modelData.isVideo && !albumRoot.itemDownloading(modelData.index)
                                                                    width: 40
                                                                    height: 40
                                                                    radius: 20
                                                                    color: Qt.rgba(0, 0, 0, 0.45)
                                                                    ColorImage {
                                                                        anchors.centerIn: parent
                                                                        source: modelData.downloaded ? "resources/play.svg" : "resources/download.svg"
                                                                        accentColor: "white"
                                                                        width: 20
                                                                        height: 20
                                                                    }
                                                                }
                                                                // Download badge for not-yet-downloaded images
                                                                Rectangle {
                                                                    anchors.centerIn: parent
                                                                    visible: modelData.isImage && !modelData.downloaded && !albumRoot.itemDownloading(modelData.index)
                                                                    width: 40
                                                                    height: 40
                                                                    radius: 20
                                                                    color: Qt.rgba(0, 0, 0, 0.45)
                                                                    ColorImage {
                                                                        anchors.centerIn: parent
                                                                        source: "resources/download.svg"
                                                                        accentColor: "white"
                                                                        width: 20
                                                                        height: 20
                                                                    }
                                                                }
                                                                // Downloading spinner
                                                                ColorImage {
                                                                    anchors.centerIn: parent
                                                                    visible: albumRoot.itemDownloading(modelData.index)
                                                                    source: "resources/loading.svg"
                                                                    accentColor: "white"
                                                                    width: 24
                                                                    height: 24
                                                                    RotationAnimation on rotation {
                                                                        running: albumRoot.itemDownloading(modelData.index)
                                                                        loops: Animation.Infinite
                                                                        from: 0
                                                                        to: 360
                                                                        duration: 900
                                                                    }
                                                                }
                                                                MouseArea {
                                                                    anchors.fill: parent
                                                                    hoverEnabled: true
                                                                    cursorShape: Qt.PointingHandCursor
                                                                    onClicked: albumRoot.activateItem(modelData)
                                                                }
                                                            }
                                                        }
                                                    }

                                                    // ---- File list ----
                                                    Column {
                                                        width: parent.width
                                                        spacing: 4
                                                        visible: albumRoot.fileItems.length > 0
                                                        Repeater {
                                                            model: albumRoot.fileItems
                                                            delegate: Rectangle {
                                                                width: parent.width
                                                                height: 48
                                                                radius: 6
                                                                // Album file-row panel — tinted above the (now dark) bubble
                                                                // so the row reads as a distinct surface.
                                                                color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.08)
                                                                Row {
                                                                    anchors.fill: parent
                                                                    anchors.margins: 8
                                                                    spacing: 10
                                                                    Rectangle {
                                                                        width: 32
                                                                        height: 32
                                                                        radius: 16
                                                                        anchors.verticalCenter: parent.verticalCenter
                                                                        color: theme.accent
                                                                        ColorImage {
                                                                            anchors.centerIn: parent
                                                                            source: modelData.downloaded
                                                                                ? delegateItem.fileTypeIcon(modelData.fileName)
                                                                                : (albumRoot.itemDownloading(modelData.index) ? "resources/loading.svg" : "resources/download.svg")
                                                                            accentColor: theme.background
                                                                            width: 16
                                                                            height: 16
                                                                            RotationAnimation on rotation {
                                                                                running: albumRoot.itemDownloading(modelData.index)
                                                                                loops: Animation.Infinite
                                                                                from: 0
                                                                                to: 360
                                                                                duration: 900
                                                                            }
                                                                        }
                                                                    }
                                                                    Column {
                                                                        anchors.verticalCenter: parent.verticalCenter
                                                                        width: parent.width - 42
                                                                        Text {
                                                                            text: modelData.fileName
                                                                            elide: Text.ElideMiddle
                                                                            width: parent.width
                                                                            color: theme.accent
                                                                            font.family: geologicaFont.name
                                                                            font.pixelSize: 13
                                                                        }
                                                                        Text {
                                                                            text: mainWindow.formatFileSize(modelData.fileSize)
                                                                            color: theme.lowAccent
                                                                            opacity: 0.7
                                                                            font.family: geologicaFont.name
                                                                            font.pixelSize: 11
                                                                        }
                                                                    }
                                                                }
                                                                MouseArea {
                                                                    anchors.fill: parent
                                                                    hoverEnabled: true
                                                                    cursorShape: Qt.PointingHandCursor
                                                                    onClicked: albumRoot.activateItem(modelData)
                                                                }
                                                            }
                                                        }
                                                    }

                                                    // ---- Caption ----
                                                    Text {
                                                        width: parent.width
                                                        visible: model.text && model.text.length > 0
                                                        // Rich text so emoji runs can render bigger + via the
                                                        // bundled color font while plain text stays untouched
                                                        // (Fonts.richText HTML-escapes all plain segments).
                                                        text: Fonts.richText(model.text, 13)
                                                        textFormat: Text.RichText
                                                        color: theme.accent
                                                        font.family: "Roboto"
                                                        font.pixelSize: 13
                                                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                                    }
                                                }
                                            }
                                            Component {
                                                id: audioContentComponent

                                                Column {
                                                    width: contentColumn.width
                                                    spacing: 8
                                                    // Nudge the voice clip down a touch when it sits under a
                                                    // reply quote, so it isn't cramped against the citation.
                                                    topPadding: bubble.hasReply ? 2 : 0

                                                    Row {
                                                        width: parent.width
                                                        spacing: 12

                                                        Rectangle {
                                                            id: audioActionButton
                                                            width: 43
                                                            height: 43
                                                            radius: width / 2
                                                            color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.14)

                                                            ColorImage {
                                                                anchors.centerIn: parent
                                                                width: 19
                                                                height: 19
                                                                source: fileTransferPending
                                                                        ? "resources/cancel.svg"
                                                                        : (!fileDownloaded
                                                                           ? "resources/download.svg"
                                                                           : ((voiceMessageManager.playing && voiceMessageManager.playingMessageId === messageId)
                                                                              ? "resources/pause.svg"
                                                                              : "resources/play.svg"))
                                                                accentColor: theme.accent
                                                            }

                                                            MouseArea {
                                                                anchors.fill: parent
                                                                cursorShape: Qt.PointingHandCursor
                                                                onClicked: {
                                                                    if (fileTransferPending) {
                                                                        if (!isOwn) {
                                                                            fileTransferManager.cancelDownload(
                                                                                contactsModel.getServer(chats.currentIndex),
                                                                                contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                                                                messageId
                                                                            )
                                                                        } else {
                                                                            fileTransferManager.cancelUpload(
                                                                                contactsModel.getServer(chats.currentIndex),
                                                                                contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                                                                messageId
                                                                            )
                                                                        }
                                                                    } else if (!fileDownloaded) {
                                                                        fileTransferManager.downloadFile(
                                                                            contactsModel.getServer(chats.currentIndex),
                                                                            contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                                                            messageId
                                                                        )
                                                                    } else {
                                                                        voiceMessageManager.togglePlayback(messageId, fileLocalPath)
                                                                    }
                                                                }
                                                            }
                                                        }

                                                        Column {
                                                            width: parent.width - 65
                                                            spacing: 4

                                                            Row {
                                                                id: audioWaveformRow
                                                                width: parent.width
                                                                spacing: 0

                                                                readonly property bool isCurrentPlaybackMessage: voiceMessageManager.playingMessageId === messageId
                                                                readonly property int waveformBarCount: 90
                                                                readonly property real playbackRatio: (isCurrentPlaybackMessage && voiceMessageManager.playbackDurationMs > 0)
                                                                    ? (voiceMessageManager.playbackPositionMs / voiceMessageManager.playbackDurationMs)
                                                                    : 0
                                                                readonly property int playedBarCount: (isCurrentPlaybackMessage && voiceMessageManager.playbackPositionMs > 0)
                                                                    ? Math.max(1, Math.ceil(playbackRatio * waveformBarCount))
                                                                    : 0

                                                                Item {
                                                                    id: waveformTrack
                                                                    width: parent.width
                                                                    height: 24
                                                                    clip: true
                                                                    readonly property int actualBarCount: (audioWaveform && audioWaveform.length > 0) ? audioWaveform.length : audioWaveformRow.waveformBarCount
                                                                    readonly property int activeBarCount: audioWaveformRow.playedBarCount
                                                                    readonly property real barSpacing: 1
                                                                    readonly property real barWidth: Math.max(1, (width - ((actualBarCount - 1) * barSpacing)) / actualBarCount)

                                                                    onActualBarCountChanged: waveformCanvas.requestPaint()
                                                                    onActiveBarCountChanged: waveformCanvas.requestPaint()

                                                                    Canvas {
                                                                        id: waveformCanvas
                                                                        anchors.fill: parent
                                                                        renderTarget: Canvas.Image
                                                                        readonly property color playedColor: theme.accent
                                                                        readonly property color idleColor: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.08)

                                                                        onWidthChanged: requestPaint()
                                                                        onHeightChanged: requestPaint()
                                                                        onAvailableChanged: requestPaint()

                                                                        onPaint: {
                                                                            const context = getContext("2d")
                                                                            context.reset()

                                                                            const count = waveformTrack.actualBarCount
                                                                            if (count <= 0 || width <= 0 || height <= 0)
                                                                                return

                                                                            for (let index = 0; index < count; index += 1) {
                                                                                const barValue = (audioWaveform && audioWaveform.length > index)
                                                                                    ? audioWaveform[index]
                                                                                    : 28
                                                                                const normalizedHeight = 8 + (Math.max(0, Math.min(100, barValue)) / 100) * 14
                                                                                const barX = index * (waveformTrack.barWidth + waveformTrack.barSpacing)
                                                                                const barY = (height - normalizedHeight) / 2
                                                                                const isPlayed = index < waveformTrack.activeBarCount

                                                                                context.fillStyle = isPlayed ? playedColor : idleColor
                                                                                context.fillRect(barX, barY, waveformTrack.barWidth, normalizedHeight)
                                                                            }
                                                                        }
                                                                    }

                                                                    MouseArea {
                                                                        anchors.fill: parent
                                                                        enabled: fileDownloaded && !fileTransferPending
                                                                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                                                        onPressed: function(mouse) {
                                                                            const ratio = width > 0 ? Math.max(0, Math.min(1, mouse.x / width)) : 0
                                                                            voiceMessageManager.seekPlayback(messageId, fileLocalPath, ratio)
                                                                        }
                                                                    }
                                                                }
                                                            }

                                                            Text {
                                                                width: parent.width
                                                                text: fileTransferPending
                                                                    ? fileTransferStatusText
                                                                    : (fileDownloaded
                                                                       ? (((voiceMessageManager.playingMessageId === messageId && voiceMessageManager.playbackDurationMs > 0)
                                                                           ? (mainWindow.formatDuration(voiceMessageManager.playbackPositionMs)
                                                                              + " / "
                                                                              + mainWindow.formatDuration(voiceMessageManager.playbackDurationMs))
                                                                           : mainWindow.formatDuration(audioDurationMs))
                                                                          + " • "
                                                                          + mainWindow.formatFileSize(fileSize))
                                                                       : qsTr("Download voice message"))
                                                                color: theme.halfAccent
                                                                opacity: 0.78
                                                                font.pixelSize: 11
                                                                font.family: geologicaFont.name
                                                                elide: Text.ElideRight
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                            Component {
                                                id: mediaContentComponent

                                                Column {
                                                    id: mediaContent
                                                    width: contentColumn.width
                                                    spacing: 8

                                                    Rectangle {
                                                        id: mediaPreview
                                                        width: parent.width
                                                        readonly property real previewAspectRatio: {
                                                            if (videoPreviewLoader.item && videoPreviewLoader.item.cachedAspectRatio > 0)
                                                                return videoPreviewLoader.item.cachedAspectRatio
                                                            if (canPreviewImage && mediaPreviewImageSource.status === Image.Ready && mediaPreviewImageSource.implicitHeight > 0)
                                                                return mediaPreviewImageSource.implicitWidth / mediaPreviewImageSource.implicitHeight
                                                            return isVideo ? (3 / 4) : (4 / 3)
                                                        }
                                                        height: {
                                                            const ratio = Math.max(0.2, previewAspectRatio)
                                                            const fittedHeight = width / ratio
                                                            return Math.max(180, Math.min(360, fittedHeight))
                                                        }
                                                        radius: 0
                                                        clip: true
                                                        smooth: true
                                                        antialiasing: true
                                                        color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.08)

                                                        readonly property bool canPreviewImage: isImage && fileDownloaded && fileLocalPath.length > 0
                                                        readonly property bool canPreviewVideo: isVideo && fileDownloaded && fileLocalPath.length > 0
                                                        readonly property string mediaUrl: mainWindow.toLocalFileUrl(fileLocalPath)

                                                        Loader {
                                                            id: videoPreviewLoader
                                                            anchors.fill: parent
                                                            active: mediaPreview.canPreviewVideo && delegateItem.isInViewport
                                                            visible: active

                                                            sourceComponent: Item {
                                                                id: videoPreviewItem
                                                                anchors.fill: parent

                                                                property real sourceRectWidth: previewVideoOutput.sourceRect.width
                                                                property real sourceRectHeight: previewVideoOutput.sourceRect.height
                                                                property real cachedAspectRatio: 0
                                                                property bool staticPreviewPauseApplied: false

                                                                AudioOutput {
                                                                    id: previewAudioOutput
                                                                    volume: 0.0
                                                                    muted: true
                                                                }

                                                                MediaPlayer {
                                                                    id: previewMediaPlayer
                                                                    source: mediaPreview.mediaUrl
                                                                    audioOutput: previewAudioOutput
                                                                    videoOutput: previewVideoOutput
                                                                    loops: MediaPlayer.Infinite
                                                                    readonly property int staticPreviewPausePositionMs: 1

                                                                    function refreshPreviewPlayback() {
                                                                        videoPreviewItem.staticPreviewPauseApplied = false

                                                                        if (mainWindow.liveMediaPreviewEnabled) {
                                                                            if (playbackState !== MediaPlayer.PlayingState)
                                                                                play()
                                                                            return
                                                                        }

                                                                        position = 0
                                                                        if (playbackState !== MediaPlayer.PlayingState)
                                                                            play()
                                                                    }

                                                                    Component.onCompleted: refreshPreviewPlayback()
                                                                    Component.onDestruction: stop()

                                                                    onPositionChanged: {
                                                                        if (!mainWindow.liveMediaPreviewEnabled
                                                                                && !videoPreviewItem.staticPreviewPauseApplied
                                                                                && playbackState === MediaPlayer.PlayingState
                                                                                && position >= staticPreviewPausePositionMs) {
                                                                            videoPreviewItem.staticPreviewPauseApplied = true
                                                                            pause()
                                                                        }
                                                                    }

                                                                    onMediaStatusChanged: {
                                                                        if (mediaStatus === MediaPlayer.LoadedMedia || mediaStatus === MediaPlayer.BufferedMedia) {
                                                                            if (mainWindow.liveMediaPreviewEnabled) {
                                                                                if (playbackState !== MediaPlayer.PlayingState)
                                                                                    play()
                                                                            } else if (!videoPreviewItem.staticPreviewPauseApplied
                                                                                    && playbackState === MediaPlayer.StoppedState) {
                                                                                play()
                                                                            }
                                                                        }
                                                                    }
                                                                }

                                                                Connections {
                                                                    target: mainWindow
                                                                    function onLiveMediaPreviewEnabledChanged() {
                                                                        previewMediaPlayer.refreshPreviewPlayback()
                                                                    }
                                                                }

                                                                VideoOutput {
                                                                    id: previewVideoOutput
                                                                    anchors.fill: parent
                                                                    visible: true
                                                                    fillMode: VideoOutput.PreserveAspectCrop

                                                                    onSourceRectChanged: {
                                                                        if (sourceRect.height > 0) {
                                                                            videoPreviewItem.cachedAspectRatio = sourceRect.width / sourceRect.height
                                                                            mainChatContainer.followBottomIfNeeded()
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }

                                                        Image {
                                                            id: mediaPreviewImageSource
                                                            anchors.fill: parent
                                                            visible: mediaPreview.canPreviewImage
                                                            source: mediaPreview.canPreviewImage ? mediaPreview.mediaUrl : ""
                                                            fillMode: Image.PreserveAspectCrop
                                                            asynchronous: true
                                                            cache: false
                                                            smooth: true
                                                            antialiasing: true
                                                            onStatusChanged: {
                                                                if (status === Image.Ready)
                                                                    mainChatContainer.followBottomIfNeeded()
                                                            }
                                                            onImplicitHeightChanged: mainChatContainer.followBottomIfNeeded()
                                                        }

                                                        Rectangle {
                                                            anchors.fill: parent
                                                            visible: !mediaPreview.canPreviewImage && !mediaPreview.canPreviewVideo
                                                            color: Qt.rgba(0, 0, 0, 0.14)
                                                            radius: 0

                                                            ColorImage {
                                                                anchors.centerIn: parent
                                                                width: 30
                                                                height: 30
                                                                source: "resources/photo.svg"
                                                                accentColor: theme.accent
                                                            }
                                                        }

                                                        Rectangle {
                                                            anchors.left: parent.left
                                                            anchors.right: parent.right
                                                            anchors.bottom: parent.bottom
                                                            z: 1
                                                            height: 52
                                                            color: Qt.rgba(0, 0, 0, 0.40)
                                                            topLeftRadius: 0
                                                            topRightRadius: 0
                                                            bottomRightRadius: 0
                                                            bottomLeftRadius: 0
                                                            antialiasing: true

                                                            Column {
                                                                anchors.fill: parent
                                                                anchors.margins: 10
                                                                spacing: 2

                                                                Row {
                                                                    width: parent.width
                                                                    spacing: 6

                                                                    Text {
                                                                        width: parent.width - (mediaFolderButton.visible ? (mediaFolderButton.width + 6) : 0)
                                                                        text: fileName.length > 0 ? fileName : model.text
                                                                        color: "white"
                                                                        font.pixelSize: 12
                                                                        font.family: geologicaFont.name
                                                                        font.weight: Font.DemiBold
                                                                        elide: Text.ElideMiddle
                                                                    }

                                                                    Item {
                                                                        id: mediaFolderButton
                                                                        width: 16
                                                                        height: 16
                                                                        visible: fileDownloaded && fileLocalPath.length > 0 && !isOwn

                                                                        ColorImage {
                                                                            anchors.centerIn: parent
                                                                            width: 14
                                                                            height: 14
                                                                            source: Platform.isMobile ? "resources/download.svg" : "resources/folder.svg"
                                                                            accentColor: "white"
                                                                        }

                                                                        MouseArea {
                                                                            anchors.fill: parent
                                                                            cursorShape: Qt.PointingHandCursor
                                                                            onClicked: {
                                                                                mouse.accepted = true
                                                                                if (Platform.isMobile)
                                                                                    mainWindow.saveFileToMobileDownloads(fileLocalPath, fileName.length > 0 ? fileName : model.text)
                                                                                else
                                                                                    fileTransferManager.openContainingFolder(fileLocalPath)
                                                                            }
                                                                        }
                                                                    }
                                                                }

                                                                Text {
                                                                    width: parent.width
                                                                    text: fileTransferPending
                                                                        ? fileTransferStatusText
                                                                        : (fileDownloaded
                                                                           ? ((isVideo ? qsTr("Video") : qsTr("Photo")) + " • " + mainWindow.formatFileSize(fileSize))
                                                                           : qsTr("Download media"))
                                                                    color: Qt.rgba(1, 1, 1, 0.82)
                                                                    font.pixelSize: 11
                                                                    font.family: geologicaFont.name
                                                                    elide: Text.ElideRight
                                                                }
                                                            }
                                                        }

                                                        Rectangle {
                                                            anchors.centerIn: parent
                                                            z: 1
                                                            width: 54
                                                            height: 54
                                                            radius: 27
                                                            visible: fileTransferPending || !fileDownloaded || isVideo
                                                            color: Qt.rgba(0, 0, 0, 0.42)

                                                            ColorImage {
                                                                anchors.centerIn: parent
                                                                width: 20
                                                                height: 20
                                                                source: fileTransferPending
                                                                        ? "resources/cancel.svg"
                                                                        : (!fileDownloaded ? "resources/download.svg" : "resources/play.svg")
                                                                accentColor: "white"
                                                            }

                                                            MouseArea {
                                                                anchors.fill: parent
                                                                cursorShape: Qt.PointingHandCursor
                                                                onClicked: {
                                                                    if (fileTransferPending) {
                                                                        if (!isOwn) {
                                                                            fileTransferManager.cancelDownload(
                                                                                contactsModel.getServer(chats.currentIndex),
                                                                                contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                                                                messageId
                                                                            )
                                                                        } else {
                                                                            fileTransferManager.cancelUpload(
                                                                                contactsModel.getServer(chats.currentIndex),
                                                                                contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                                                                messageId
                                                                            )
                                                                        }
                                                                    } else if (!fileDownloaded) {
                                                                        fileTransferManager.downloadFile(
                                                                            contactsModel.getServer(chats.currentIndex),
                                                                            contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                                                            messageId
                                                                        )
                                                                    } else {
                                                                        mainWindow.openMediaViewer(
                                                                            fileLocalPath,
                                                                            isVideo,
                                                                            fileName.length > 0 ? fileName : model.text
                                                                        )
                                                                    }
                                                                }
                                                            }
                                                        }

                                                        MouseArea {
                                                            anchors.fill: parent
                                                            enabled: !fileTransferPending && fileDownloaded
                                                            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                                            onClicked: {
                                                                mainWindow.openMediaViewer(
                                                                    fileLocalPath,
                                                                    isVideo,
                                                                    fileName.length > 0 ? fileName : model.text
                                                                )
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                            Component {
                                                id: fileContentComponent

                                                Column {
                                                    width: contentColumn.width
                                                    spacing: 6

                                                    Row {
                                                        width: parent.width
                                                        spacing: 12

                                                        Item {
                                                            id: fileCircleItem
                                                            width: 52
                                                            height: 52
                                                            readonly property color actionColor: theme.accent
                                                            readonly property color actionFillColor: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.12)

                                                            Shape {
                                                                anchors.fill: parent
                                                                visible: fileTransferPending
                                                                layer.enabled: true
                                                                layer.samples: 4
                                                                ShapePath {
                                                                    strokeWidth: 3
                                                                    strokeColor: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.18)
                                                                    fillColor: "transparent"
                                                                    capStyle: ShapePath.RoundCap
                                                                    startX: fileCircleItem.width / 2
                                                                    startY: 4
                                                                    PathAngleArc {
                                                                        centerX: fileCircleItem.width / 2
                                                                        centerY: fileCircleItem.height / 2
                                                                        radiusX: fileCircleItem.width / 2 - 4
                                                                        radiusY: fileCircleItem.height / 2 - 4
                                                                        startAngle: -90
                                                                        sweepAngle: 360
                                                                    }
                                                                }
                                                                ShapePath {
                                                                    strokeWidth: 3
                                                                    strokeColor: fileCircleItem.actionColor
                                                                    fillColor: "transparent"
                                                                    capStyle: ShapePath.RoundCap
                                                                    startX: fileCircleItem.width / 2
                                                                    startY: 4
                                                                    PathAngleArc {
                                                                        centerX: fileCircleItem.width / 2
                                                                        centerY: fileCircleItem.height / 2
                                                                        radiusX: fileCircleItem.width / 2 - 4
                                                                        radiusY: fileCircleItem.height / 2 - 4
                                                                        startAngle: -90
                                                                        sweepAngle: Math.max(6, 360 * Math.max(0.02, fileTransferProgress))
                                                                    }
                                                                }
                                                            }

                                                            Rectangle {
                                                                anchors.centerIn: parent
                                                                width: 40
                                                                height: 40
                                                                radius: 20
                                                                readonly property bool staticOwnFileIcon: isOwn && fileDownloaded && !fileTransferPending
                                                                visible: fileTransferPending || !fileDownloaded || !isOwn || staticOwnFileIcon
                                                                color: "transparent"

                                                                ColorImage {
                                                                    anchors.centerIn: parent
                                                                    width: 25
                                                                    height: 25
                                                                    source: fileTransferPending
                                                                            ? "resources/cancel.svg"
                                                                                : (parent.staticOwnFileIcon
                                                                                   ? delegateItem.fileTypeIcon(fileName.length > 0 ? fileName : model.text)
                                                                                   : (fileDownloaded
                                                                                   ? (Platform.isMobile ? "resources/download.svg" : "resources/folder.svg")
                                                                                   : "resources/download.svg"))
                                                                    accentColor: parent.parent.actionColor
                                                                }

                                                                MouseArea {
                                                                    anchors.fill: parent
                                                                    cursorShape: Qt.PointingHandCursor
                                                                    enabled: parent.visible && !parent.staticOwnFileIcon
                                                                    onClicked: {
                                                                        if (fileTransferPending) {
                                                                            if (!isOwn) {
                                                                                fileTransferManager.cancelDownload(
                                                                                    contactsModel.getServer(chats.currentIndex),
                                                                                    contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                                                                    messageId
                                                                                )
                                                                            } else {
                                                                                fileTransferManager.cancelUpload(
                                                                                    contactsModel.getServer(chats.currentIndex),
                                                                                    contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                                                                    messageId
                                                                                )
                                                                            }
                                                                        } else if (fileDownloaded) {
                                                                            if (Platform.isMobile)
                                                                                mainWindow.saveFileToMobileDownloads(fileLocalPath, fileName.length > 0 ? fileName : model.text)
                                                                            else
                                                                                fileTransferManager.openContainingFolder(fileLocalPath)
                                                                        } else {
                                                                            fileTransferManager.downloadFile(
                                                                                contactsModel.getServer(chats.currentIndex),
                                                                                contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                                                                messageId
                                                                            )
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }

                                                        Column {
                                                            width: parent.width - 64
                                                            spacing: 4

                                                            Text {
                                                                text: fileName.length > 0 ? fileName : model.text
                                                                color: theme.accent
                                                                font.pixelSize: 12
                                                                font.family: geologicaFont.name
                                                                font.weight: Font.DemiBold
                                                                elide: Text.ElideMiddle
                                                                width: parent.width
                                                            }
                                                            Text {
                                                                text: mainWindow.formatFileSize(fileSize)
                                                                color: theme.halfAccent
                                                                opacity: 0.75
                                                                font.pixelSize: 11
                                                                font.family: geologicaFont.name
                                                            }
                                                            Text {
                                                                width: parent.width
                                                                text: fileTransferPending
                                                                    ? fileTransferStatusText
                                                                    : ((isOwn && fileDownloaded)
                                                                       ? ""
                                                                       : (fileDownloaded
                                                                       ? (Platform.isMobile ? qsTr("Save to Downloads") : qsTr("Open folder"))
                                                                       : qsTr("Download")))
                                                                color: theme.halfAccent
                                                                opacity: 0.8
                                                                font.pixelSize: 11
                                                                font.family: geologicaFont.name
                                                                elide: Text.ElideRight
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                            Component {
                                                id: textContentComponent

                                                Row {
                                                    width: contentColumn.width
                                                    spacing: 8

                                                    Text {
                                                        // Rich text so emoji runs can render bigger + via the
                                                        // bundled color font while plain text stays untouched
                                                        // (Fonts.richText HTML-escapes all plain segments).
                                                        text: Fonts.richText(model.text, 14)
                                                        textFormat: Text.RichText
                                                        color: theme.accent
                                                        font.family: "Roboto"
                                                        font.pixelSize: 14
                                                        width: parent.width
                                                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                                    }
                                                }
                                            }
                                            Component {
                                                id: callContentComponent

                                                Row {
                                                    width: contentColumn.width
                                                    spacing: 0

                                                    Row {
                                                        spacing: bubble.callRowSpacing

                                                        Column {
                                                            spacing: 4

                                                            Text {
                                                                text: isMissedCall ? qsTr("Missed Call") : (isOwn ? qsTr("Outgoing Call") : qsTr("Incoming Call"))
                                                                color: theme.accent
                                                                font.pixelSize: 14
                                                                font.family: geologicaFont.name
                                                                font.weight: Font.Normal
                                                            }
                                                            Row {
                                                                spacing: bubble.callIconTimeSpacing

                                                                ColorImage {
                                                                    width: bubble.callArrowIconSize
                                                                    height: bubble.callArrowIconSize
                                                                    anchors.verticalCenter: parent.verticalCenter
                                                                    source: isOwn ? "resources/arrow-up-right.svg" : "resources/arrow-down-left.svg"
                                                                    accentColor: isMissedCall ? theme.redAccent : (theme.accent)
                                                                }
                                                                Text {
                                                                    text: model.callDurationSec > 0
                                                                        ? (model.time + " · " + mainWindow.formatDuration(model.callDurationSec * 1000))
                                                                        : model.time
                                                                    font.pixelSize: 10
                                                                    color: theme.halfAccent
                                                                    opacity: 0.7
                                                                    anchors.verticalCenter: parent.verticalCenter
                                                                }
                                                            }
                                                        }
                                                        ColorImage {
                                                            width: bubble.callSecondColumnIconSize
                                                            height: bubble.callSecondColumnIconSize
                                                            anchors.verticalCenter: parent.verticalCenter
                                                            source: "resources/call.svg"
                                                            accentColor: theme.accent
                                                        }
                                                    }
                                                    Item {
                                                        width: bubble.callIconTrailingMargin
                                                        height: 1
                                                    }
                                                }
                                            }
                                            Row { // time and status
                                                visible: !isCall
                                                anchors.right: parent.right
                                                spacing: 4
                                                Text { // time
                                                    text: model.time
                                                    font.pixelSize: 10
                                                    color: theme.halfAccent
                                                    opacity: 0.7
                                                }
                                                ColorImage { // message status
                                                    visible: isOwn
                                                    width: 12; height: 12
                                                    accentColor: theme.accent
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    source: isSended ? (isReaded ? "resources/double_checked.svg" : (isReceived ? "resources/checked.svg" : "resources/internet.svg"))
                                                                    : "resources/sending.svg"
                                                    
                                                    effectMaskSpreadAtMin: 1.2
                                                    effectMaskThresholdMin: 0.55

                                                    MouseArea {
                                                        anchors.fill: parent
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            stack.push(messageStatusInfoPage)
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        } // Rectangle
                        // Active reply draft (the message currently being quoted
                        // in the composer). Populated from the message context menu
                        // or a swipe-to-reply, cleared on send or cancel.
                        QtObject {
                            id: replyDraft
                            property bool active: false
                            property double replyToId: 0
                            property string preview: ""
                            property string kind: ""
                            property bool isOwn: false
                            function start(id, own, k, p) {
                                replyToId = id
                                isOwn = own
                                kind = k || ""
                                preview = p || ""
                                active = true
                                messageInput.forceActiveFocus()
                            }
                            function clear() {
                                active = false
                                replyToId = 0
                                preview = ""
                                kind = ""
                                isOwn = false
                            }
                        }

                        // Files chosen but not yet sent. They preview above the
                        // input; the caption typed in the input is attached to the
                        // album on send.
                        ListModel {
                            id: attachmentStaging
                        }
                        Connections {
                            target: fileTransferManager
                            function onFilesPicked(serverId, contactPubKey, items) {
                                if (serverId !== contactsModel.getServer(chats.currentIndex)
                                        || contactPubKey !== contactsModel.getPubKeyFingerprint(chats.currentIndex))
                                    return
                                for (var i = 0; i < items.length; i++)
                                    attachmentStaging.append(items[i])
                            }
                            function onSaveToDownloadsRequested(localPath, fileName) {
                                mainWindow.saveToDownloads(localPath, fileName)
                            }
                        }

                        // Strips shown above the input bar: the reply quote and
                        // (added later) the attachment staging area. Height is 0
                        // when nothing is active, so the input sits flush as before.
                        Column {
                            id: composeExtras
                            readonly property bool floating: mainPagePage.isMobileLayout
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: floating ? 8 : 5
                            anchors.rightMargin: floating ? 8 : 5
                            anchors.bottom: chatInputBar.top
                            // Lift off the input pill so the strips read as separate
                            // floating cards instead of fusing with the composer.
                            anchors.bottomMargin: floating ? 8 : 0
                            spacing: floating ? 6 : 0

                            Rectangle {
                                id: stagingStrip
                                width: parent.width
                                visible: attachmentStaging.count > 0
                                height: visible ? 88 : 0
                                radius: composeExtras.floating ? 18 : 0
                                color: composeExtras.floating ? mainWindow.chatPanelSurface : theme.background

                                PanelBorder {
                                    visible: composeExtras.floating
                                    edgeColor: mainWindow.chatPanelBorderColor
                                }

                                ListView {
                                    id: stagingList
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    orientation: ListView.Horizontal
                                    spacing: 8
                                    clip: true
                                    model: attachmentStaging
                                    delegate: Item {
                                        width: (model.isImage || model.isVideo) ? 72 : 184
                                        height: 72

                                        Rectangle {
                                            anchors.fill: parent
                                            radius: 8
                                            color: theme.minimumAccent
                                            clip: true

                                            Image {
                                                anchors.fill: parent
                                                visible: model.isImage
                                                source: model.isImage ? mainWindow.toLocalFileUrl(model.path) : ""
                                                fillMode: Image.PreserveAspectCrop
                                                asynchronous: true
                                                cache: false
                                            }
                                            ColorImage {
                                                anchors.centerIn: parent
                                                visible: model.isVideo
                                                source: "resources/video.svg"
                                                accentColor: theme.accent
                                                width: 26
                                                height: 26
                                            }
                                            Row {
                                                visible: !model.isImage && !model.isVideo
                                                anchors.fill: parent
                                                anchors.margins: 8
                                                spacing: 8
                                                ColorImage {
                                                    source: "resources/file.svg"
                                                    accentColor: theme.accent
                                                    width: 24
                                                    height: 24
                                                    anchors.verticalCenter: parent.verticalCenter
                                                }
                                                Column {
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    width: parent.width - 32
                                                    Text {
                                                        text: model.name
                                                        elide: Text.ElideRight
                                                        width: parent.width
                                                        color: theme.accent
                                                        font.family: geologicaFont.name
                                                        font.pixelSize: 12
                                                    }
                                                    Text {
                                                        text: mainWindow.formatFileSize(model.size)
                                                        color: theme.halfAccent
                                                        font.family: geologicaFont.name
                                                        font.pixelSize: 10
                                                    }
                                                }
                                            }
                                        }
                                        Rectangle {
                                            width: 20
                                            height: 20
                                            radius: 10
                                            color: theme.background
                                            border.color: theme.aroundZeroAccent
                                            anchors.top: parent.top
                                            anchors.right: parent.right
                                            MouseArea {
                                                anchors.fill: parent
                                                onClicked: attachmentStaging.remove(index)
                                            }
                                            ColorImage {
                                                source: "resources/close.svg"
                                                accentColor: theme.accent
                                                width: 10
                                                height: 10
                                                anchors.centerIn: parent
                                            }
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                id: replyStrip
                                width: parent.width
                                visible: replyDraft.active
                                height: visible ? 48 : 0
                                radius: composeExtras.floating ? 18 : 0
                                color: composeExtras.floating ? mainWindow.chatPanelSurface : theme.background

                                PanelBorder {
                                    visible: composeExtras.floating
                                    edgeColor: mainWindow.chatPanelBorderColor
                                }

                                ColorImage {
                                    id: replyStripIcon
                                    source: "resources/reply.svg"
                                    accentColor: theme.accent
                                    width: 20
                                    height: 20
                                    anchors.left: parent.left
                                    anchors.leftMargin: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Rectangle {
                                    id: replyStripBar
                                    width: 2
                                    radius: 1
                                    height: 30
                                    color: theme.accent
                                    anchors.left: replyStripIcon.right
                                    anchors.leftMargin: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                MouseArea {
                                    id: replyStripClose
                                    width: 34
                                    height: 34
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    onClicked: replyDraft.clear()
                                    ColorImage {
                                        source: "resources/close.svg"
                                        accentColor: theme.halfAccent
                                        width: 12
                                        height: 12
                                        anchors.centerIn: parent
                                    }
                                }
                                Column {
                                    anchors.left: replyStripBar.right
                                    anchors.leftMargin: 10
                                    anchors.right: replyStripClose.left
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2
                                    Text {
                                        text: mainWindow.replyAuthorName(replyDraft.isOwn, contactsModel.getFirstName(chats.currentIndex), contactsModel.getLastName(chats.currentIndex))
                                        color: theme.accent
                                        font.family: geologicaFont.name
                                        font.pixelSize: 12
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                        width: parent.width
                                    }
                                    Text {
                                        text: mainWindow.replyLabel(replyDraft.kind, replyDraft.preview)
                                        color: theme.halfAccent
                                        font.family: geologicaFont.name
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                        maximumLineCount: 1
                                        width: parent.width
                                    }
                                }
                                // Tapping the strip (anywhere but the close button) jumps to
                                // the quoted message, same pulse as tapping a reply bubble.
                                MouseArea {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    anchors.right: replyStripClose.left
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: messagesList.jumpToRepliedMid(replyDraft.replyToId)
                                }
                            }
                        }

                        Rectangle {
                            id: topBorder
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: composeExtras.top
                            // Hidden on mobile: the floating composer pill provides
                            // its own separation, a full-width rule would cut across it.
                            visible: !mainPagePage.isMobileLayout
                            height: visible ? 1 : 0
                            color: theme.aroundZeroAccent
                        }

                        // Rounded raised surface behind the composer (mobile only).
                        // Declared before chatInputBar so it paints behind the row;
                        // padded out past the row so the buttons sit inside the pill.
                        Rectangle {
                            id: inputBarBg
                            visible: mainPagePage.isMobileLayout
                            anchors.fill: chatInputBar
                            anchors.topMargin: -4
                            anchors.bottomMargin: -4
                            anchors.leftMargin: -6
                            // No right overhang: the pill ends at the composer's right
                            // edge so the accent send circle lands flush in the corner.
                            anchors.rightMargin: 0
                            // Full pill when the field is a single line, then hold that
                            // same corner radius as the input grows to multiple lines
                            // (instead of tracking height/2 and re-rounding). +4 = the
                            // top overhang, so this is half the collapsed pill height.
                            radius: Math.min(height / 2, attachButton.height / 2 + 4)
                            color: mainWindow.chatPanelSurface

                            PanelBorder {
                                edgeColor: mainWindow.chatPanelBorderColor
                            }
                        }
                        RowLayout {
                            id: chatInputBar
                            // Android IME reset logic удалена

                            // Insert a picked emoji at the caret (or at the end if the
                            // field was never focused), then keep the caret after it.
                            function insertEmoji(emoji) {
                                var pos = messageInput.cursorPosition
                                if (pos < 0 || pos > messageInput.length)
                                    pos = messageInput.length
                                // An emoji-only line renders with a visibly shorter cursor and
                                // field height than a mixed line — Qt computes a line's cursor/
                                // content metrics only from the font engines actually present ON
                                // it, and the bundled color-emoji fallback's own metrics run
                                // smaller than the primary Latin font. Anchoring the very first
                                // emoji into an empty field with an invisible zero-width space
                                // keeps a normal-metrics character on that line, so it never
                                // shrinks in the first place — stripped everywhere it matters
                                // (view.visibleEditorText, the placeholder) and never sent.
                                if (messageInput.text.length === 0) {
                                    messageInput.insert(pos, "\u200B")
                                    pos += 1
                                }
                                messageInput.insert(pos, emoji)
                            }

                            function sendCurrentMessage() {
                                // Capture the text (incl. any IME pre-edit) BEFORE committing:
                                // committing can transiently empty visibleEditorText (pre-edit
                                // cleared before `text` catches up), which on mobile made a
                                // send tap dismiss the keyboard yet drop the message.
                                const hasAttachments = attachmentStaging.count > 0
                                const oldText = view.visibleEditorText.trim()
                                Qt.inputMethod.commit()
                                if (!hasAttachments && oldText.length === 0)
                                    return

                                const contactPubKey = contactsModel.getPubKeyFingerprint(chats.currentIndex)
                                const contactServer = contactsModel.getServer(chats.currentIndex)

                                // Snapshot + consume the reply draft before clearing it.
                                const rActive = replyDraft.active
                                const rTo = rActive ? replyDraft.replyToId : 0
                                const rPreview = rActive ? replyDraft.preview : ""
                                const rKind = rActive ? replyDraft.kind : ""
                                const rIsOwn = rActive ? replyDraft.isOwn : false

                                // Staged files go as one album; the input text (if
                                // any) becomes the album's caption.
                                if (hasAttachments) {
                                    var paths = []
                                    for (var i = 0; i < attachmentStaging.count; i++)
                                        paths.push(attachmentStaging.get(i).path)
                                    messageInput.clear()
                                    attachmentStaging.clear()
                                    replyDraft.clear()
                                    fileTransferManager.sendAlbum(contactServer, contactPubKey, paths, oldText, rTo, rPreview, rKind, rIsOwn)
                                    messagesList.positionViewAtEnd()
                                    return
                                }

                                messageInput.clear()
                                const mid = messageModel.uniqId()
                                const messageTimestamp = Math.floor(Date.now() / 1000)
                                // Snapshot the disappearing-message timer now and send it
                                // with the message so the recipient stores the same TTL we
                                // do (see connectionpayloadprocessor: "timer" field).
                                const msgTimer = contactsModel.getTimerValue(chats.currentIndex)
                                console.log("mid = "   + mid)
                                replyDraft.clear()

                                messageModel.sendMessage(mid, contactServer, contactPubKey, oldText, messageTimestamp, rTo, rPreview, rKind, rIsOwn)
                                messagesList.positionViewAtEnd()
                                contactsModel.setLastMessageAt(contactServer, contactPubKey, oldText, messageTimestamp)
                                MainSignals.emitMessageSended(mid, contactServer, contactPubKey, oldText, messageTimestamp, rTo, rPreview, rKind, rIsOwn)
                                Qt.callLater(function() {
                                    var payload = {
                                        type: "text",
                                        timestamp: messageTimestamp,
                                        text: oldText.trim(),
                                        message_id: mid,
                                        timer: msgTimer
                                    }
                                    if (rTo && rTo > 0) {
                                        payload.reply_to = rTo
                                        payload.reply_preview = rPreview
                                        payload.reply_kind = rKind
                                        payload.reply_author_is_sender = rIsOwn
                                    }
                                    connections.e2eSendJSON(contactServer,
                                        contactPubKey,
                                        JSON.stringify(payload),
                                        function(status) {
                                            if (status === "delivered") {
                                                console.log("Message " + mid + " delivered successfully")
                                                messageModel.updateMessageSendedStatus(contactServer, contactPubKey, mid)
                                                messageModel.updateMessageDeliveredStatus(contactServer, contactPubKey, mid)
                                                MainSignals.emitMessageStatusDelivered(mid, contactServer, contactPubKey)
                                            } else if (status === "received") {
                                                console.log("Server received message " + mid)
                                                messageModel.updateMessageSendedStatus(contactServer, contactPubKey, mid)
                                                MainSignals.emitMessageStatusSended(mid, contactServer, contactPubKey)
                                            }
                                        }
                                    )
                                })
                            }
                            visible: !mainChatContainer.currentContactRevoked
                            // True while the soft keyboard is up (either signal source).
                            readonly property bool keyboardVisible: (AndroidSystemUi.available && AndroidSystemUi.keyboardHeight > 0) || Qt.inputMethod.visible
                            anchors.bottom: parent.bottom
                            // Extra bottom clearance on mobile so the floating pill
                            // (inputBarBg, which extends 6px past the row) clears the
                            // gesture/nav area instead of hugging the screen edge —
                            // and a slightly larger gap so it doesn't stick to the
                            // keyboard when it's open (16 base + 6 to offset the pill).
                            anchors.bottomMargin: mainPagePage.isMobileLayout ? (keyboardVisible ? 30 : 22) : 0
                            Behavior on anchors.bottomMargin { NumberAnimation { duration: 120 } }
                            anchors.left: parent.left
                            anchors.right: parent.right

                            // Left: 14 − 6px inputBarBg overhang ≈ 8px gap (matches the
                            // header). Right: 8 with no overhang, so the pill edge — and the
                            // accent send circle nested in its right cap — sits flush at 8px.
                            anchors.leftMargin: mainPagePage.isMobileLayout ? 14 : 5
                            anchors.rightMargin: mainPagePage.isMobileLayout ? 8 : 5

                            BottomButton {
                                id: attachButton
                                Layout.preferredWidth: 36
                                Layout.minimumWidth: 36
                                Layout.maximumWidth: 36
                                Layout.preferredHeight: Platform.isMobile ? 38 : 36
                                Layout.minimumHeight: Platform.isMobile ? 38 : 36
                                Layout.maximumHeight: Platform.isMobile ? 38 : 36
                                Layout.alignment: Qt.AlignBottom // bottom button
                                Layout.leftMargin: Platform.isMobile ? 2 : 0
                                Layout.rightMargin: Platform.isMobile ? 1 : 0
                                icon.source: voiceMessageManager.recording ? "resources/cancel.svg" : "resources/attach.svg"
                                icon.color: voiceMessageManager.recording ? theme.redAccent : (view.visibleEditorText.trim().length === 0 || attachmentStaging.count > 0) ? theme.accent : theme.lowAccent
                                icon.width: Platform.isMobile ? 32 : 25
                                icon.height: Platform.isMobile ? 32 : 25
                                enabled: voiceMessageManager.recording || attachmentStaging.count > 0 || view.visibleEditorText.trim().length === 0

                                onClicked: {
                                    if (chats.currentIndex === -1)
                                        return

                                    if (voiceMessageManager.recording) {
                                        voiceMessageManager.cancelRecording()
                                        return
                                    }

                                    if (attachOptionsPopup.visible)
                                        attachOptionsPopup.close()
                                    else
                                        attachOptionsPopup.open()
                                }

                                // Desktop: open the attach menu on hover. Mobile has no hover
                                // events, so it only opens via onClicked above.
                                HoverHandler {
                                    id: attachButtonHover
                                    enabled: !Platform.isMobile
                                    onHoveredChanged: {
                                        if (hovered) {
                                            if (chats.currentIndex !== -1 && !voiceMessageManager.recording)
                                                attachOptionsPopup.open()
                                        } else {
                                            attachHoverCloseTimer.restart()
                                        }
                                    }
                                }

                                // Grace period so moving the pointer from the button into the
                                // popup doesn't close it before the popup's own hover takes over.
                                Timer {
                                    id: attachHoverCloseTimer
                                    interval: 150
                                    onTriggered: {
                                        if (!attachButtonHover.hovered && !attachPopupHover.hovered)
                                            attachOptionsPopup.close()
                                    }
                                }

                                Popup {
                                    id: attachOptionsPopup
                                    parent: attachButton
                                    x: 0
                                    y: -height - 6
                                    width: 180
                                    height: attachOptionsColumn.implicitHeight
                                    padding: 0
                                    modal: false
                                    focus: false
                                    closePolicy: Popup.CloseOnPressOutsideParent | Popup.CloseOnEscape

                                    HoverHandler {
                                        id: attachPopupHover
                                        enabled: !Platform.isMobile
                                        onHoveredChanged: {
                                            if (!hovered)
                                                attachHoverCloseTimer.restart()
                                        }
                                    }

                                    // Same treatment as the other floating panels (composer
                                    // pill, emoji picker): tinted surface + tapered PanelBorder
                                    // rim instead of a flat uniform border.
                                    background: Rectangle {
                                        radius: 12
                                        color: mainWindow.chatPanelSurface

                                        PanelBorder {
                                            edgeColor: mainWindow.chatPanelBorderColor
                                        }
                                    }

                                    contentItem: ColumnLayout {
                                        id: attachOptionsColumn
                                        spacing: 0

                                        ItemDelegate {
                                            id: attachFileItem
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 42
                                            padding: 0
                                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                                            background: Rectangle {
                                                radius: 8
                                                color: attachFileItem.hovered ? Qt.rgba(1, 1, 1, 0.03) : "transparent"
                                            }
                                            contentItem: RowLayout {
                                                spacing: 10
                                                anchors.fill: parent
                                                anchors.leftMargin: 14
                                                anchors.rightMargin: 14
                                                ColorImage {
                                                    source: "resources/attach.svg"
                                                    accentColor: theme.accent
                                                    Layout.preferredWidth: 18
                                                    Layout.preferredHeight: 18
                                                    Layout.alignment: Qt.AlignVCenter
                                                }
                                                Text {
                                                    text: qsTr("Attach file")
                                                    font.family: geologicaFont.name
                                                    font.pixelSize: 12
                                                    color: theme.accent
                                                    Layout.fillWidth: true
                                                    Layout.alignment: Qt.AlignVCenter
                                                    horizontalAlignment: Text.AlignLeft
                                                    elide: Text.ElideRight
                                                }
                                            }
                                            onClicked: {
                                                attachOptionsPopup.close()
                                                fileTransferManager.pickFilesForStaging(
                                                    contactsModel.getServer(chats.currentIndex),
                                                    contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                                    false
                                                )
                                            }
                                        }
                                        ItemDelegate {
                                            id: attachMediaItem
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 42
                                            padding: 0
                                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                                            background: Rectangle {
                                                radius: 8
                                                color: attachMediaItem.hovered ? Qt.rgba(1, 1, 1, 0.03) : "transparent"
                                            }
                                            contentItem: RowLayout {
                                                spacing: 10
                                                anchors.fill: parent
                                                anchors.leftMargin: 14
                                                anchors.rightMargin: 14
                                                ColorImage {
                                                    source: "resources/photo.svg"
                                                    accentColor: theme.accent
                                                    Layout.preferredWidth: 18
                                                    Layout.preferredHeight: 18
                                                    Layout.alignment: Qt.AlignVCenter
                                                }
                                                Text {
                                                    text: qsTr("Attach media")
                                                    font.family: geologicaFont.name
                                                    font.pixelSize: 12
                                                    color: theme.accent
                                                    Layout.fillWidth: true
                                                    Layout.alignment: Qt.AlignVCenter
                                                    horizontalAlignment: Text.AlignLeft
                                                    elide: Text.ElideRight
                                                }
                                            }
                                            onClicked: {
                                                attachOptionsPopup.close()
                                                fileTransferManager.pickFilesForStaging(
                                                    contactsModel.getServer(chats.currentIndex),
                                                    contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                                    true
                                                )
                                            }
                                        }
                                    }
                                }
                            }

                            // Rectangle {
                            //     id: bottomBorder
                            //     anchors {
                            //         bottom: parent.bottom
                            //         left: parent.left
                            //         right: parent.right
                            //     }
                            //     height: 1
                            //     radius: 10
                            //     color: theme.lowestAccent
                            // }

                            BottomButton { // emoji picker toggle — sits just right of attach
                                id: emojiButton
                                visible: !voiceMessageManager.recording
                                Layout.preferredWidth: 36
                                Layout.minimumWidth: 36
                                Layout.maximumWidth: 36
                                Layout.preferredHeight: Platform.isMobile ? 38 : 36
                                Layout.minimumHeight: Platform.isMobile ? 38 : 36
                                Layout.maximumHeight: Platform.isMobile ? 38 : 36
                                Layout.alignment: Qt.AlignBottom
                                Layout.leftMargin: Platform.isMobile ? 1 : 0
                                Layout.rightMargin: Platform.isMobile ? 1 : 0
                                // Mobile: the panel replaces the keyboard, so while it's open
                                // the button swaps to a keyboard glyph — a tap on it closes the
                                // panel and brings the real keyboard back. Desktop keeps the smile
                                // (the picker floats above, there's no keyboard to return to).
                                icon.source: (Platform.isMobile && emojiPicker.visible) ? "resources/keyboard.svg" : "resources/smile.svg"
                                // Always available (unlike attach, which disables while typing),
                                // so it always reads as active/accent-colored.
                                icon.color: theme.accent
                                icon.width: Platform.isMobile ? 26 : 20
                                icon.height: Platform.isMobile ? 26 : 20

                                onClicked: {
                                    if (chats.currentIndex === -1)
                                        return

                                    if (Platform.isMobile) {
                                        // Mobile: the picker docks at the bottom and REPLACES the
                                        // keyboard (like Telegram) instead of floating above this
                                        // button — toggling it means swapping which one occupies
                                        // that screen space.
                                        if (emojiPicker.visible) {
                                            emojiPicker.close()
                                            messageInput.forceActiveFocus()
                                            if (Platform.isAndroid && AndroidSystemUi.available)
                                                messageInput.requestAndroidKeyboardAfterTap()
                                        } else {
                                            // Capture the keyboard's current (or last-known) height
                                            // so the panel replaces it at the same size with no
                                            // layout jump. When the keyboard was NEVER shown this
                                            // session, `stack`'s bottomMargin starts at 0 and would
                                            // otherwise animate up to match — but the panel itself
                                            // appears instantly, so for one tick that animation lag
                                            // left the panel overlapping the (not-yet-repositioned)
                                            // composer. Suppress it for this one jump so both move
                                            // together; real keyboard-height changes (e.g. the
                                            // crossfade on close, below) still animate normally.
                                            // Floored at 220 HERE (matching the panel's own minimum
                                            // height below) rather than only where the panel reads
                                            // it — if the keyboard was dismissed by the OS/user just
                                            // before this tap (not via this button), keyboardHeight
                                            // can still be caught mid-hide-animation at some small
                                            // in-between value; without the floor here, `stack`
                                            // would reserve that smaller amount while the panel
                                            // still renders at its 220 floor, leaving the composer
                                            // partly hidden behind it.
                                            mainWindow.suppressBottomMarginAnimation = true
                                            mainWindow.emojiPanelHeight = Math.max(220, AndroidSystemUi.keyboardHeight > 0
                                                    ? AndroidSystemUi.keyboardHeight
                                                    : (mainWindow.lastKnownKeyboardHeight > 0 ? mainWindow.lastKnownKeyboardHeight : 280))
                                            // Drop focus so Qt's Android integration hides the real
                                            // keyboard, but keep the caret rendered — it reads as
                                            // "still in the field", just typing via the panel instead.
                                            messageInput.focus = false
                                            messageInput.cursorVisible = true
                                            emojiPicker.open()
                                            mainWindow.suppressBottomMarginAnimation = false
                                        }
                                        return
                                    }

                                    // Desktop: a small floating bubble above this button.
                                    if (emojiPicker.visible)
                                        emojiPicker.close()
                                    else
                                        emojiPicker.open()
                                    // Only reclaim focus if the tap actually took it away.
                                    if (!messageInput.activeFocus)
                                        messageInput.forceActiveFocus()
                                }

                                EmojiPicker {
                                    id: emojiPicker
                                    // Desktop: a small bubble floating above the smile button.
                                    // Mobile: docks full-width at the bottom of the window,
                                    // replacing the keyboard's screen space (mainWindow.
                                    // emojiPanelHeight, reserved in `stack`'s bottomMargin above).
                                    parent: Platform.isMobile ? mainWindow.contentItem : emojiButton
                                    // Mobile: inset 8px from the left edge (paired with the
                                    // width below) so the docked panel doesn't sit flush to
                                    // the screen and matches the right-side gutter.
                                    x: Platform.isMobile ? 8 : 0
                                    // Mobile: shift up by safeBottomInset so the panel's bottom
                                    // edge lands where the nav bar starts, mirroring the real
                                    // keyboard (a system surface that always sits above the nav
                                    // bar). Without this, the docked panel — anchored flush to
                                    // contentItem's raw bottom — runs under the opaque 3-button
                                    // nav bar with no breathing room; gesture nav has a near-zero
                                    // safeBottomInset so the bug was invisible there. Add a little
                                    // extra (button-nav only, where safeBottomInset is non-zero) so
                                    // the panel doesn't just kiss the nav bar's top edge.
                                    y: Platform.isMobile ? (parent.height - height - mainWindow.safeBottomInset - (mainWindow.safeBottomInset > 0 ? 12 : 0)) : (-height - 8)
                                    // Mobile: full width minus an 8px gutter on each side
                                    // (x: 8 above), so the panel stays clear of the screen
                                    // edges instead of running edge-to-edge / off-screen.
                                    width: Platform.isMobile ? parent.width - 16 : 348
                                    height: Platform.isMobile ? Math.max(220, mainWindow.emojiPanelHeight) : 296
                                    onClosed: {
                                        // Covers every dismiss path (re-tapping smile, tapping the
                                        // input, tapping outside via closePolicy) so the reserved
                                        // space always collapses back, not just the explicit toggle.
                                        if (Platform.isMobile)
                                            mainWindow.emojiPanelHeight = 0
                                    }
                                    onPicked: function(emoji) {
                                        chatInputBar.insertEmoji(emoji)
                                        if (!Platform.isMobile && !messageInput.activeFocus) {
                                            messageInput.forceActiveFocus()
                                            if (Platform.isAndroid && AndroidSystemUi.available)
                                                messageInput.requestAndroidKeyboardAfterTap()
                                        }
                                    }
                                }
                            }

                            Item { // message input container
                                id: view
                                // The committed text with the in-progress IME pre-edit spliced
                                // in AT THE CURSOR, not appended. When the caret is moved back
                                // into the middle of the field (e.g. "Г| Б В" while "Г" is still
                                // composing), preeditText belongs at cursorPosition — plain
                                // `text + preeditText` would reorder it to "Б ВГ" on send.
                                readonly property string visibleEditorText: {
                                    var t = messageInput.text
                                    var p = messageInput.preeditText
                                    var result
                                    if (p.length === 0)
                                        result = t
                                    else {
                                        var pos = messageInput.cursorPosition
                                        if (pos < 0 || pos > t.length)
                                            pos = t.length
                                        result = t.slice(0, pos) + p + t.slice(pos)
                                    }
                                    // Strip the zero-width-space metrics anchor (see
                                    // chatInputBar.insertEmoji) — purely a local rendering aid,
                                    // never meant to count toward "is the field empty" here or
                                    // reach the actual sent message text.
                                    return result.replace(/\u200B/g, "")
                                }
                                Layout.fillWidth: true
                                Layout.minimumWidth: 50
                                Layout.minimumHeight: Platform.isMobile ? 38 : 36
                                Layout.maximumHeight: 120
                                // Cached height — updated imperatively so Layout.preferredHeight only
                                // changes when lines actually change, not on every cursor X-movement.
                                property real _cachedH: Platform.isMobile ? 38 : 36
                                function _updateH() {
                                    var cr = messageInput.cursorRectangle
                                    var cursorBottom = cr.y + cr.height + messageInput.bottomPadding
                                    var h = Math.max(messageInput.implicitHeight, cursorBottom)
                                    _cachedH = Math.min(Math.max(Platform.isMobile ? 38 : 36, h), 120)
                                }
                                Connections {
                                    target: messageInput
                                    function onCursorRectangleChanged() { view._updateH() }
                                    function onImplicitHeightChanged() { view._updateH() }
                                }
                                Layout.preferredHeight: _cachedH
                                clip: true
                                // When input field grows (multi-line typing), keep messages scrolled to bottom.
                                onHeightChanged: {
                                    if (Platform.isMobile)
                                        mainChatContainer.followBottomIfNeeded()
                                }
                                ScrollView {
                                    id: messageInputScrollView
                                    anchors.fill: parent
                                    contentWidth: width
                                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                                TextArea {
                                    id: messageInput
                                    width: messageInputScrollView.width
                                    selectedTextColor: theme.background
                                    // Material's own CursorDelegate hardwires its visibility to
                                    // `parent.activeFocus` alone — it never looks at
                                    // `cursorVisible` at all, so defocusing the field (done above
                                    // to hide the real keyboard while the mobile emoji panel is
                                    // open) hid the caret too, with no way to override it via the
                                    // `cursorVisible` property. This mirrors Material's own
                                    // CursorDelegate.qml exactly, just OR-ing in `cursorVisible` so
                                    // it also shows/blinks while the panel has taken the keyboard's
                                    // place — so it's still obvious where the next emoji lands.
                                    cursorDelegate: Rectangle {
                                        id: caret
                                        color: parent.Material.accentColor
                                        width: 2
                                        // Hidden while recording: the field is disabled and the
                                        // recording timer/waveform takes its place, but cursorVisible
                                        // can still be latched true (forced on when the emoji panel
                                        // opened, and record can be started straight from that panel),
                                        // which otherwise left the caret blinking over the waveform.
                                        visible: (parent.activeFocus || parent.cursorVisible)
                                                 && !parent.readOnly
                                                 && !voiceMessageManager.recording
                                                 && parent.selectionStart === parent.selectionEnd

                                        Connections {
                                            target: caret.parent
                                            function onCursorPositionChanged() {
                                                caret.opacity = 1
                                                blinkTimer.restart()
                                            }
                                        }
                                        Timer {
                                            id: blinkTimer
                                            running: (caret.parent.activeFocus || caret.parent.cursorVisible)
                                                     && !caret.parent.readOnly && interval !== 0
                                            repeat: true
                                            interval: Application.styleHints.cursorFlashTime / 2
                                            onTriggered: caret.opacity = !caret.opacity ? 1 : 0
                                            onRunningChanged: caret.opacity = 1
                                        }
                                    }
                                    leftPadding: 10
                                    rightPadding: 10
                                    topPadding: Platform.isMobile ? 9 : 10
                                    bottomPadding: Platform.isMobile ? 9 : 10
                                    enabled: !voiceMessageManager.recording
                                    persistentSelection: true
                                    property bool androidLongPressActive: false
                                    property int _savedSelStart: 0
                                    property int _savedSelEnd: 0
                                    property bool _selExistedAtPress: false
                                    property real _selectionAppearedAt: 0

                                    // Text emoticon → emoji autoreplace (":)" → 🙂, "<3" → ❤️, …).
                                    // Longer patterns are listed first so ">:(" wins over ":(".
                                    property bool _autoReplacing: false
                                    readonly property var _emoticons: [
                                        [">:(", "😠"], [":-)", "🙂"], [":-(", "🙁"], [":-D", "😀"],
                                        [":'(", "😢"], ["</3", "💔"], ["<3", "❤️"], [":)", "🙂"],
                                        [":(", "🙁"], [":D", "😀"], [";)", "😉"], [":P", "😛"],
                                        [":p", "😛"], [":o", "😮"], [":O", "😮"], ["8)", "😎"],
                                        [":*", "😘"], ["xD", "😆"], ["XD", "😆"], [":|", "😐"]
                                    ]
                                    onTextChanged: {
                                        // Skip our own edits and in-progress IME composition.
                                        if (_autoReplacing || preeditText.length > 0)
                                            return
                                        var pos = cursorPosition
                                        if (pos <= 0)
                                            return
                                        var before = text.substring(0, pos)
                                        for (var i = 0; i < _emoticons.length; i++) {
                                            var pat = _emoticons[i][0]
                                            if (before.length >= pat.length
                                                    && before.substring(before.length - pat.length) === pat) {
                                                var startIdx = pos - pat.length
                                                // Only fire on a word boundary so we don't mangle
                                                // things like "http:)" or ":D" mid-word.
                                                var prev = startIdx > 0 ? text.charAt(startIdx - 1) : " "
                                                if (startIdx === 0 || prev === " " || prev === "\n" || prev === "\t") {
                                                    _autoReplacing = true
                                                    remove(startIdx, pos)
                                                    insert(startIdx, _emoticons[i][1])
                                                    _autoReplacing = false
                                                }
                                                break
                                            }
                                        }
                                    }

                                    // Restores selection after Qt clears it on short-long-press touch-up.
                                    // Two-stage: Qt.callLater handles the immediate 1ms reset;
                                    // this timer handles the Android gesture-cleanup oscillations ~180ms later.
                                    Timer {
                                        id: selectionRestoreTimer
                                        interval: 250
                                        onTriggered: {
                                            console.log("[SEL] restoreTimer: sel=", messageInput._savedSelStart, "-", messageInput._savedSelEnd)
                                            messageInput.select(messageInput._savedSelStart, messageInput._savedSelEnd)
                                        }
                                    }

                                    onSelectionStartChanged: {
                                        console.log("[SEL] selectionStart=", selectionStart, "selectionEnd=", selectionEnd)
                                        if (selectionStart !== selectionEnd && _selectionAppearedAt === 0)
                                            _selectionAppearedAt = Date.now()
                                        else if (selectionStart === selectionEnd)
                                            _selectionAppearedAt = 0
                                    }
                                    onSelectionEndChanged: console.log("[SEL] selectionEnd=", selectionEnd, "selectionStart=", selectionStart)

                                    function requestAndroidKeyboardAfterTap() {
                                        if (!(Platform.isAndroid && AndroidSystemUi.available))
                                            return

                                        console.log("[SEL] requestAndroidKeyboardAfterTap: sel=", selectionStart, "-", selectionEnd)
                                        Qt.callLater(function() {
                                            console.log("[SEL] callLater: activeFocus=", activeFocus, "sel=", selectionStart, "-", selectionEnd)
                                            if (messageInput.activeFocus && messageInput.selectionStart === messageInput.selectionEnd) {
                                                console.log("[SEL] → showKeyboardWithHints")
                                                mainWindow.lastKeyboardTarget = messageInput
                                                AndroidSystemUi.showKeyboardWithHints(messageInput.inputMethodHints)
                                            }
                                        })
                                    }

                                    onPressed: function() {
                                        // Tapping the input to type dismisses the docked mobile
                                        // emoji panel (if open) and brings the real keyboard back.
                                        if (Platform.isMobile && emojiPicker.visible)
                                            emojiPicker.close()

                                        androidLongPressActive = false
                                        selectionRestoreTimer.stop()
                                        _savedSelStart = 0
                                        _savedSelEnd = 0
                                        _selExistedAtPress = (selectionStart !== selectionEnd)
                                        _selectionAppearedAt = 0
                                        console.log("[SEL] onPressed: sel=", selectionStart, "-", selectionEnd, "selExisted=", _selExistedAtPress)
                                    }

                                    onPressAndHold: function() {
                                        androidLongPressActive = true
                                        console.log("[SEL] onPressAndHold: sel=", selectionStart, "-", selectionEnd)
                                    }

                                    // Only request the IME after a normal tap release.
                                    // Long-press selection must finish without any focus or IME churn.
                                    onReleased: function() {
                                        console.log("[SEL] onReleased: longPressActive=", androidLongPressActive,
                                                    "activeFocus=", messageInput.activeFocus,
                                                    "sel=", selectionStart, "-", selectionEnd)
                                        if (!Platform.isAndroid || !AndroidSystemUi.available)
                                            return

                                        if (androidLongPressActive)
                                            return

                                        // Qt internally clears selection on touch-up when onPressAndHold
                                        // hasn't fired yet (< 800ms hold). Restore only if:
                                        // 1. Selection was CREATED during this touch (_selExistedAtPress=false)
                                        // 2. Selection was visible ≥ 175ms before release (filters accidental
                                        //    500ms holds where Qt selects just before user lifts finger)
                                        var selDurationMs = (_selectionAppearedAt > 0)
                                                ? (Date.now() - _selectionAppearedAt) : 0
                                        if (messageInput.selectionStart !== messageInput.selectionEnd
                                                && !_selExistedAtPress && selDurationMs >= 175) {
                                            _savedSelStart = messageInput.selectionStart
                                            _savedSelEnd = messageInput.selectionEnd
                                            console.log("[SEL] short-LPR: saving sel=", _savedSelStart, "-", _savedSelEnd, "duration=", selDurationMs, "ms")
                                            Qt.callLater(function() {
                                                console.log("[SEL] callLater restore: cur=", messageInput.selectionStart, "-", messageInput.selectionEnd)
                                                messageInput.select(_savedSelStart, _savedSelEnd)
                                            })
                                            selectionRestoreTimer.restart()
                                            return
                                        }

                                        if (!messageInput.activeFocus)
                                            messageInput.forceActiveFocus(Qt.OtherFocusReason)

                                        // If selection existed before this tap, Qt will clear it on touch-up
                                        // (sel is still non-zero here at onReleased time). Use callLater so
                                        // the check runs after Qt places the cursor (sel becomes 0-0).
                                        if (_selExistedAtPress) {
                                            Qt.callLater(function() {
                                                if (!messageInput.inputMethodComposing && messageInput.selectionStart === messageInput.selectionEnd)
                                                    requestAndroidKeyboardAfterTap()
                                            })
                                            return
                                        }

                                        if (!messageInput.inputMethodComposing && messageInput.selectionStart === messageInput.selectionEnd)
                                            requestAndroidKeyboardAfterTap()
                                    }

                                    Menu {
                                        id: optionsMenu2
                                        y: parent.height
                                        topInset: 0
                                        bottomInset: 0
                                        padding: 0
                                        property bool leftAlign: false
                                        property bool isRed: false
                                        background: Rectangle {
                                            implicitWidth: 200
                                            color: mainWindow.chatPanelSurface
                                            border.color: mainWindow.chatPanelBorderColor
                                            border.width: 1
                                            radius: 14

                                            PanelBorder {
                                                edgeColor: mainWindow.chatPanelBorderColor
                                            }
                                        }
                                        MyMenuItem { // Cut
                                            text: qsTr("Cut")
                                            onTriggered: {
                                                if (messageInput.selectionStart === messageInput.selectionEnd)
                                                    return

                                                messageInput.cut()
                                            }
                                            leftAlign: optionsMenu2.leftAlign
                                            isRed: true
                                            width: 200
                                            hotKey: "Ctrl+X"
                                            colorAccent: theme.accent
                                            colorBackground: theme.background
                                            colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                            iconSource: "resources/cut.svg"
                                        }
                                        MyMenuItem { // Copy
                                            text: qsTr("Copy")
                                            onTriggered: {
                                                if (messageInput.selectionStart === messageInput.selectionEnd)
                                                    return

                                                messageInput.copy()
                                            }
                                            leftAlign: optionsMenu2.leftAlign
                                            width: 200
                                            hotKey: "Ctrl+C"
                                            colorAccent: theme.accent
                                            colorBackground: theme.background
                                            colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                            iconSource: "resources/copy.svg"
                                        }
                                        MyMenuItem { // Paste
                                            text: qsTr("Paste")
                                            width: 200
                                            leftAlign: optionsMenu2.leftAlign
                                            hotKey: "Ctrl+V"
                                            onTriggered: {
                                                messageInput.forceActiveFocus()
                                                messageInput.paste()
                                                if (Platform.isAndroid && AndroidSystemUi.available)
                                                    messageInput.requestAndroidKeyboardAfterTap()
                                            }
                                            colorAccent: theme.accent
                                            colorBackground: theme.background
                                            colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                            iconSource: "resources/paste.svg"
                                        }
                                        MenuSeparator {
                                            contentItem: Rectangle {
                                                implicitHeight: 1
                                                color: theme.background
                                                opacity: 0.2
                                            }
                                        }
                                        MyMenuItem { // Select All
                                            text: qsTr("Select All")
                                            onTriggered: messageInput.selectAll()
                                            leftAlign: optionsMenu2.leftAlign
                                            width: 200
                                            hotKey: "Ctrl+A"
                                            colorAccent: theme.accent
                                            colorBackground: theme.background
                                            colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                            iconSource: "resources/selectall.svg"
                                        }
                                        MyMenuItem { // Copy All
                                            text: qsTr("Copy All")
                                            onTriggered: ClipboardHelper.setText(messageInput.text)
                                            leftAlign: optionsMenu2.leftAlign
                                            width: 200
                                            hotKey: "Ctrl+X"
                                            hotKeyVisible: false
                                            colorAccent: theme.accent
                                            colorBackground: theme.background
                                            colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                            iconSource: "resources/copy.svg"
                                        }
                                        MyMenuItem { // Clear
                                            text: qsTr("Clear")
                                            onTriggered: messageInput.text = ""
                                            leftAlign: optionsMenu2.leftAlign
                                            width: 200
                                            isRed: true
                                            hotKey: "Ctrl+X"
                                            hotKeyVisible: false
                                            colorAccent: theme.accent
                                            colorBackground: theme.background
                                            colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                            iconSource: "resources/clear.svg"
                                        }
                                        MenuSeparator {
                                            contentItem: Rectangle {
                                                implicitHeight: 1
                                                color: theme.background
                                                opacity: 0.2
                                            }
                                        }
                                        MyMenuItem { // Undo
                                            text: qsTr("Undo")
                                            onTriggered: messageInput.undo()
                                            leftAlign: optionsMenu2.leftAlign
                                            width: 200
                                            isRed: true
                                            hotKey: "Ctrl+Z"
                                            colorAccent: theme.accent
                                            colorBackground: theme.background
                                            colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                            iconSource: "resources/undo.svg"
                                        }
                                        MyMenuItem { // Redo
                                            text: qsTr("Redo")
                                            onTriggered: messageInput.redo()
                                            leftAlign: optionsMenu2.leftAlign
                                            width: 200
                                            hotKey: "Ctrl+Y"
                                            colorAccent: theme.accent
                                            colorBackground: theme.background
                                            colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                            iconSource: "resources/redo.svg"
                                        }
                                    }
                                    inputMethodHints: Qt.ImhMultiLine
                                    // Emoji fallback in the composer too (matches message text).
                                    font: Fonts.emoji("Roboto", Platform.isMobile ? 15 : 14, Font.Normal)
                                    color: theme.accent
                                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                    onCursorRectangleChanged: {
                                        if (!activeFocus) return
                                        var fl = messageInputScrollView.contentItem
                                        if (!fl || fl.height <= 0) return
                                        var bp = messageInput.bottomPadding
                                        function doScroll() {
                                            var h = fl.height
                                            if (fl.contentY > cursorRectangle.y)
                                                fl.contentY = cursorRectangle.y
                                            else if (fl.contentY + h < cursorRectangle.y + cursorRectangle.height + bp)
                                                fl.contentY = Math.max(0, cursorRectangle.y + cursorRectangle.height + bp - h)
                                        }
                                        // At max height fl.height is stable — scroll synchronously (no frame lag).
                                        // While growing, fl.height updates in polish; defer until after that.
                                        if (view._cachedH >= 120)
                                            doScroll()
                                        else
                                            Qt.callLater(doScroll)
                                    }
                                    background: Rectangle {
                                        color: "transparent"
                                        radius: 0
                                    }
                                    Material.foreground: theme.accent
                                    Material.background: "transparent"
                                    
                                    // ИСПРАВИТЬ: Существующий MouseArea для правой кнопки
                                    MouseArea {
                                        anchors.fill: parent
                                        // On Android Qt.RightButton is emitted for long-press.
                                        // Enabling this on mobile would intercept long-press and
                                        // prevent native TextArea text selection from starting.
                                        enabled: Platform.isDesktop
                                        acceptedButtons: Qt.RightButton
                                        cursorShape: Qt.IBeamCursor
                                        onClicked: {
                                            optionsMenu2.popup()
                                        }
                                    }
                                    
                                    Item {
                                        id: messageInputPlaceholder
                                        width: parent.width - 20
                                        height: placeholderColumn.height
                                        anchors.left: parent.left
                                        anchors.leftMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        // .replace strips the zero-width-space metrics anchor (see
                                        // chatInputBar.insertEmoji) so a field holding only that
                                        // invisible character still shows the placeholder.
                                        visible: messageInput.text.replace(/\u200B/g, "").length === 0 && messageInput.preeditText.length === 0

                                        Column {
                                            id: placeholderColumn
                                            width: parent.width
                                            spacing: voiceMessageManager.recording ? 0 : 0

                                            Text {
                                                text: voiceMessageManager.recording
                                                    ? qsTr("Recording voice message %1").arg(mainWindow.formatDuration(voiceMessageManager.recordingDurationMs))
                                                    : qsTr("Type a message...")
                                                width: parent.width
                                                color: theme.lowAccent
                                                font.pixelSize: Platform.isMobile ? 14 : 13
                                                font.family: "Roboto"
                                                font.weight: Font.Normal
                                                elide: Text.ElideRight
                                            }

                                            Item {
                                                id: recordingIndicator
                                                visible: voiceMessageManager.recording
                                                width: parent.width
                                                height: Platform.isMobile ? 14 : 20
                                                property bool mobileBlinkVisible: true
                                                readonly property var recordingBars: voiceMessageManager.recordingLevelBars
                                                readonly property real mobileRecordingLevel: {
                                                    const bars = recordingBars
                                                    if (!bars || bars.length === 0)
                                                        return 0.08

                                                    let peak = 0
                                                    const startIndex = Math.max(0, bars.length - 6)
                                                    for (let i = startIndex; i < bars.length; ++i)
                                                        peak = Math.max(peak, Number(bars[i]) || 0)

                                                    return Math.max(0.08, Math.min(1.0, peak / 100.0))
                                                }

                                                Rectangle {
                                                    visible: Platform.isMobile
                                                    anchors.left: parent.left
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    width: 14
                                                    height: 14
                                                    color: "transparent"

                                                    Rectangle {
                                                        anchors.centerIn: parent
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        width: 10
                                                        height: 10
                                                        radius: 5
                                                        color: theme.redAccent
                                                        visible: recordingIndicator.mobileBlinkVisible
                                                    }

                                                    Timer {
                                                        interval: 520
                                                        repeat: true
                                                        running: voiceMessageManager.recording && Platform.isMobile
                                                        onRunningChanged: {
                                                            if (!running)
                                                                recordingIndicator.mobileBlinkVisible = true
                                                        }
                                                        onTriggered: recordingIndicator.mobileBlinkVisible = !recordingIndicator.mobileBlinkVisible
                                                    }
                                                }

                                                Row {
                                                    visible: !Platform.isMobile
                                                    anchors.fill: parent
                                                    spacing: 0

                                                    Repeater {
                                                        model: voiceMessageManager.recordingLevelBars

                                                        Rectangle {
                                                            required property int index
                                                            required property var modelData

                                                            readonly property real fadeRatio: 48 > 1 ? (index / 47) : 1
                                                            readonly property real barOpacity: 0.00 + fadeRatio * 0.32

                                                            width: Math.max(1, parent.width / 48)
                                                            height: 5 + (Math.max(0, Math.min(100, modelData)) / 100) * 10
                                                            radius: 0
                                                            anchors.bottom: parent.bottom
                                                            color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, barOpacity)
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            // Same as above: disabled on mobile to allow native text selection.
                                            enabled: Platform.isDesktop
                                            acceptedButtons: Qt.RightButton
                                            cursorShape: Qt.IBeamCursor
                                            onClicked: {
                                                optionsMenu2.x = mouse.x
                                                optionsMenu2.y = mouse.y
                                                optionsMenu2.leftAlign = true
                                                optionsMenu2.open()
                                            }
                                        }
                                    }
                                    Keys.onReturnPressed: (event) => {
                                        if (Platform.isDesktop && !(event.modifiers & Qt.ShiftModifier)) {
                                            event.accepted = true
                                            chatInputBar.sendCurrentMessage()
                                            return
                                        }
                                        event.accepted = false
                                    }
                                }
                                } // ScrollView
                                // imeResetFocusProxy удалён
                            }
                            Connections {
                                target: Platform.isAndroid && AndroidSystemUi.available ? AndroidSystemUi : null
                                function onKeyboardHeightChanged() {
                                    // When keyboard slides up, scroll message list to bottom
                                    // so the last message isn't hidden behind the input bar.
                                    if (AndroidSystemUi.keyboardHeight > 0) {
                                        mainChatContainer.followBottomIfNeeded()
                                        // Remembered so the emoji panel can match the keyboard's
                                        // height even if opened before the keyboard ever showed.
                                        mainWindow.lastKnownKeyboardHeight = AndroidSystemUi.keyboardHeight
                                    }
                                }
                            }
                            Connections { // !!!!!! onTextMessageReceived
                                target: connections
                                function onTextMessageReceived(serverId, fromPubKey, message, mid) {
                                    //messageModel.receiveMessage(mid, fromPubKey, message);
                                    if (serverId === contactsModel.getServer(chats.currentIndex) && fromPubKey === contactsModel.getPubKeyFingerprint(chats.currentIndex)) {
                                        if (Settings.getBoolSetting("soundsEnabled") && (Settings.getBoolSetting("soundFromActiveChatEnabled"))) {
                                            console.log("Playing notification sound for active chat")
                                            notificationCurrentChatSound.play()
                                        }
                                        connections.sendReadedSignal(contactsModel.getServer(chats.currentIndex), fromPubKey);
                                        messagesList.positionViewAtEnd()
                                        MainSignals.emitMessageReaded(serverId, fromPubKey, mid);
                                    } else {
                                        if (!Platform.isMobile && Settings.getBoolSetting("soundsEnabled")) {
                                            console.log("Playing notification sound for inactive chat")
                                            notificationSound.play()
                                        }
                                        // Skip the tray notification while the offline
                                        // backlog is being replayed on (re)connect -- those
                                        // are old messages the user is opening the app to
                                        // read, not fresh arrivals. The unread count below
                                        // still updates.
                                        if (Settings.getBoolSetting("notificationsEnabled")
                                                && !mainWindow.isSyncingBacklog()) {
                                            AppNotifier.showNotification(
                                                qsTr("New message"),
                                                qsTr("You have received a new message")
                                            )
                                        }
                                        contactsModel.incrementUnreadedCount(serverId, fromPubKey);
                                    }
                                }
                            }
                            BottomButton {
                                id: voiceButton
                                Layout.preferredWidth: Platform.isMobile ? 46 : 36
                                Layout.minimumWidth: Platform.isMobile ? 46 : 36
                                Layout.maximumWidth: Platform.isMobile ? 46 : 36
                                Layout.preferredHeight: Platform.isMobile ? 38 : 36
                                Layout.minimumHeight: Platform.isMobile ? 38 : 36
                                Layout.maximumHeight: Platform.isMobile ? 38 : 36
                                Layout.alignment: Qt.AlignBottom
                                // Mobile: the mic yields to the accent send circle once
                                // there's something to send (desktop keeps both visible).
                                visible: !Platform.isMobile || (view.visibleEditorText.trim().length === 0 && attachmentStaging.count === 0)
                                icon.source: voiceMessageManager.recording ? "resources/send.svg" : "resources/mic.svg"
                                // Mobile: dark glyph on the accent circle (same as send).
                                icon.color: Platform.isMobile ? theme.background : ((view.visibleEditorText.trim().length === 0 && attachmentStaging.count === 0) ? theme.accent : theme.lowAccent)
                                icon.width: Platform.isMobile ? 24 : 20
                                icon.height: Platform.isMobile ? 24 : 20
                                enabled: view.visibleEditorText.trim().length === 0 && attachmentStaging.count === 0

                                // Accent circle (mobile): same as the send button, so the mic
                                // and send share one persistent accent circle at the pill's
                                // right cap — 40 = single-line pill height (46) minus a 3px
                                // inset on top/right/bottom; pinned to the bottom-anchored
                                // button, never grows with the input.
                                Rectangle {
                                    z: -1
                                    visible: Platform.isMobile
                                    width: 40
                                    height: 40
                                    radius: height / 2
                                    color: theme.accent
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.verticalCenter: parent.verticalCenter
                                }

                                onClicked: {
                                    if (chats.currentIndex === -1)
                                        return

                                    if (voiceMessageManager.recording) {
                                        // Attach + consume the active reply, so the voice message
                                        // is sent as a reply and the quote clears from the input.
                                        var rTo = replyDraft.active ? replyDraft.replyToId : 0
                                        var rPreview = replyDraft.active ? replyDraft.preview : ""
                                        var rKind = replyDraft.active ? replyDraft.kind : ""
                                        var rIsOwn = replyDraft.active ? replyDraft.isOwn : false
                                        replyDraft.clear()
                                        voiceMessageManager.stopRecordingAndSend(
                                            contactsModel.getServer(chats.currentIndex),
                                            contactsModel.getPubKeyFingerprint(chats.currentIndex),
                                            rTo, rPreview, rKind, rIsOwn
                                        )
                                    } else {
                                        // Close the emoji panel before recording. The smile button
                                        // hides while recording, but the picker Popup is a separate
                                        // surface (docked at the bottom on mobile) that would stay
                                        // open — its taps kept inserting emoji into the field mid-
                                        // recording. Closing it also collapses the reserved panel
                                        // space via emojiPicker.onClosed.
                                        emojiPicker.close()
                                        voiceMessageManager.startRecording()
                                    }
                                }
                            }
                            BottomButton { // send button
                                id: sendButton
                                Layout.preferredWidth: Platform.isMobile ? 46 : 36
                                Layout.minimumWidth: Platform.isMobile ? 46 : 36
                                Layout.maximumWidth: Platform.isMobile ? 46 : 36
                                Layout.preferredHeight: Platform.isMobile ? 38 : 36
                                Layout.minimumHeight: Platform.isMobile ? 38 : 36
                                Layout.maximumHeight: Platform.isMobile ? 38 : 36
                                Layout.alignment: Qt.AlignBottom // bottom button
                                Layout.rightMargin: 0
                                icon.source: "resources/send.svg"
                                // Mobile: dark glyph on the accent circle. Desktop: accent glyph, no circle.
                                icon.color: Platform.isMobile
                                    ? (sendButton.enabled ? theme.background : theme.lowAccent)
                                    : ((!voiceMessageManager.recording && (view.visibleEditorText.trim().length > 0 || attachmentStaging.count > 0)) ? theme.accent : theme.lowAccent)
                                icon.width: Platform.isMobile ? 24 : 20
                                icon.height: Platform.isMobile ? 24 : 20
                                enabled: !voiceMessageManager.recording && (view.visibleEditorText.trim().length > 0 || attachmentStaging.count > 0)
                                // Mobile: only when there's something to send (mic shows otherwise).
                                visible: !voiceMessageManager.recording && (!Platform.isMobile || view.visibleEditorText.trim().length > 0 || attachmentStaging.count > 0)

                                // Accent send circle (mobile): 40 = single-line pill height
                                // (38 row + 2×4 overhang = 46) minus a 3px inset on top/right/
                                // bottom; vertically centered on the bottom-pinned button, so it
                                // sits inside the pill and never grows as the input expands.
                                Rectangle {
                                    z: -1
                                    visible: Platform.isMobile && sendButton.enabled
                                    width: 40
                                    height: 40
                                    radius: height / 2
                                    color: theme.accent
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.verticalCenter: parent.verticalCenter
                                }

                                // Enlarged tap target: the input bar sits a few px
                                // above the screen edge on mobile, so taps landing just
                                // below/around the icon used to miss. This extends the
                                // hit area (mostly downward, not left into the mic).
                                MouseArea {
                                    anchors.fill: parent
                                    anchors.topMargin: -10
                                    anchors.bottomMargin: -18
                                    anchors.rightMargin: -8
                                    anchors.leftMargin: 0
                                    enabled: sendButton.enabled
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: chatInputBar.sendCurrentMessage()
                                }
                            } // BottomButton
                        } // RowLayout
                        Item { // hacked notice - shown in place of the input bar for a revoked contact
                            visible: mainChatContainer.currentContactRevoked
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: Platform.isMobile ? 12 : 0
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 5
                            anchors.rightMargin: 5
                            height: Math.max(Platform.isMobile ? 38 : 36, hackedNoticeRow.implicitHeight + 12)
                            RowLayout {
                                id: hackedNoticeRow
                                anchors.centerIn: parent
                                width: parent.width - 20
                                spacing: 8
                                ColorImage {
                                    source: "resources/hacker.svg"
                                    accentColor: theme.redAccent
                                    Layout.preferredWidth: 18
                                    Layout.preferredHeight: 18
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                Text {
                                    text: qsTr("This contact was hacked. You can no longer message them.")
                                    color: theme.redAccent
                                    font.pixelSize: 12
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                }
                            }
                        }
                    } // Item mainChatContainer
                    Rectangle { // rightBarResizer
                        id: rightBarResizer
                        readonly property bool activeResizer: !mainPagePage.isMobileLayout && mainWindow.width >= 1200 && rightBarPanel.panelVisible
                        Layout.preferredWidth: activeResizer ? 2 : 0
                        Layout.minimumWidth: activeResizer ? 2 : 0
                        Layout.maximumWidth: activeResizer ? 2 : 0
                        Layout.fillHeight: true
                        Layout.alignment: Qt.AlignRight
                        color: "transparent"
                        opacity: 1.0
                        layer.enabled: true
                        visible: activeResizer
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "transparent" }
                            GradientStop { position: 0.5; color: theme.lowestAccent }
                            GradientStop { position: 1.0; color: "transparent" }
                            orientation: Gradient.Vertical
                        }
                        MouseArea {
                            cursorShape: Qt.SizeHorCursor
                            height: parent.height
                            anchors.centerIn: parent
                            preventStealing: true
                            width: pressed ? 2000 : parent.width
                            property real startX
                            property real startWidth
                            onPressed: {
                                startX = mapToItem(null, mouse.x, 0).x
                                startWidth = mainPagePage.rightBarWidth
                            }
                            onPositionChanged: {
                                var mouseX = mapToItem(null, mouse.x, 0).x
                                let newWidth = startWidth - (mouseX - startX)
                                newWidth = Math.max(350, Math.min(550, newWidth))
                                mainPagePage.rightBarWidth = newWidth
                            }
                        }
                    }
                    Rectangle { // rightBarPanel
                        id: rightBarPanel
                        readonly property bool panelVisible: chats.currentIndex !== -1 && (mainPagePage.isMobileLayout ? mainPagePage.mobilePane === mainPagePage.mobilePaneInfo : mainWindow.width >= 1200)
                        Layout.preferredWidth: panelVisible ? (mainPagePage.isMobileLayout ? parent.width : rightBarWidth) : 0
                        Layout.minimumWidth: panelVisible ? (mainPagePage.isMobileLayout ? 0 : rightBarWidth) : 0
                        Layout.maximumWidth: panelVisible ? (mainPagePage.isMobileLayout ? parent.width : rightBarWidth) : 0
                        Layout.fillHeight: true
                        color: "transparent"
                        Layout.alignment: Qt.AlignRight
                        visible: panelVisible
                        ScrollView {
                            id: additionalPanelScroll
                            anchors.fill: parent
                            contentWidth: availableWidth
                            contentHeight: additionalPanel.implicitHeight + (mainPagePage.isMobileLayout ? 20 : 40)
                            WheelScroller { flickable: additionalPanelScroll.contentItem }
                            ScrollBar.vertical: ScrollBar {
                                policy: ScrollBar.AsNeeded
                                width: 6
                                minimumSize: 0.1
                                interactive: true
                                contentItem: Rectangle {
                                    radius: 3
                                    color: theme.accent
                                    opacity: 0.2
                                }
                                background: Rectangle {
                                    radius: 0
                                    color: "transparent"
                                    opacity: 0.1
                                }
                            }
                            ScrollBar.horizontal: null
                            ColumnLayout {
                                id: additionalPanel
                                width: parent.width - 20
                                x: 10
                                y: mainPagePage.isMobileLayout ? 10 : 30
                                spacing: 0
                                Rectangle {
                                    visible: mainPagePage.isMobileLayout
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 44
                                    color: theme.background

                                    BackButton {
                                        id: backButton
                                        anchors.top: parent.top
                                        anchors.left: parent.left
                                        anchors.topMargin: 10
                                        anchors.leftMargin: 20
                                        
                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: mainWindow.navigateBack()
                                        }
                                    }

                                    // Text {
                                    //     anchors.centerIn: parent
                                    //     text: qsTr("Contact info")
                                    //     color: theme.accent
                                    //     font.pixelSize: 15
                                    //     font.family: geologicaFont.name
                                    //     font.weight: Font.DemiBold
                                    // }
                                }
                                Rectangle { // avatar
                                    id: avatarContainer2
                                    Layout.preferredWidth: 70
                                    Layout.preferredHeight: 70
                                    Layout.topMargin: 30
                                    Layout.alignment: Qt.AlignTop | Qt.AlignHCenter
                                    radius: width / 2
                                    color: theme.minimumAccent
                                    layer.enabled: true
                                    Text { // ! move to variables
                                        id: avatarLetter2
                                        anchors.centerIn: parent
                                        text: contactsModel.getFirstName(chats.currentIndex)[0]
                                        font.pixelSize: 34
                                        font.family: geologicaFont.name
                                        color: theme.accent
                                        visible: !contactsModel.getAvatarSource(chats.currentIndex).startsWith("image://avatars/")
                                    }
                                    Connections { // !
                                        target: contactsModel
                                        function onContactUpdated(index) {
                                            if (index === chats.currentIndex) {
                                                avatarLetter2.text = contactsModel.getFirstName(chats.currentIndex)[0]
                                                // Refresh visibility too: a live avatar arrival must
                                                // hide this letter (the binding above only re-runs when
                                                // the open chat changes, not on dataChanged).
                                                avatarLetter2.visible = !contactsModel.getAvatarSource(chats.currentIndex).startsWith("image://avatars/")
                                            }
                                        }
                                    }
                                    Image {
                                        id: avatarImg2
                                        anchors.fill: parent
                                        visible: false
                                        source: Account.getContactAvatarSource(contactsModel.getServer(chats.currentIndex), contactsModel.getPubKeyFingerprint(chats.currentIndex))
                                        mipmap: true
                                        fillMode: Image.PreserveAspectCrop
                                        smooth: true
                                        antialiasing: true
                                        layer.enabled: true
                                        layer.smooth: true
                                    }      
                                    Connections { // !!!
                                        target: connections
                                        function onContactAvatarChanged(updatedServerId, updatedPubKeyFingerprint) {
                                            if (updatedServerId === contactsModel.getServer(chats.currentIndex)
                                                    && updatedPubKeyFingerprint === contactsModel.getPubKeyFingerprint(chats.currentIndex)) {
                                                avatarImg2.source = Account.getContactAvatarSource(contactsModel.getServer(chats.currentIndex), contactsModel.getPubKeyFingerprint(chats.currentIndex)) + "?t=" + Date.now();
                                            }
                                        }
                                    }
                                    Rectangle { // avatar mask
                                        id: mask2
                                        anchors.fill: parent
                                        radius: avatarContainer2.radius
                                        visible: false
                                        layer.enabled: true
                                        layer.smooth: true
                                    }
                                    MultiEffect { // avatar circle effect
                                        id: avatarMaskEffect2
                                        anchors.fill: parent
                                        source: avatarImg2
                                        maskEnabled: true
                                        maskSource: mask2
                                        // startsWith (not length > 0): the anonymous placeholder
                                        // ("resources/avatars/anonymous.svg") also has length > 0 but
                                        // isn't a real avatar, so the old check showed an empty masked
                                        // circle for contacts with no avatar set.
                                        visible: contactsModel.getAvatarSource(chats.currentIndex).startsWith("image://avatars/")
                                        smooth: true
                                        antialiasing: true
                                        maskSpreadAtMin: 1.0
                                        maskThresholdMin: 0.6
                                    }
                                    Connections { // !
                                        target: contactsModel
                                        function onContactUpdated(index) {
                                            if (index === chats.currentIndex) {
                                                avatarMaskEffect2.visible = contactsModel.getAvatarSource(chats.currentIndex).startsWith("image://avatars/")
                                            }
                                        }
                                    }
                                    Rectangle {
                                        id: onlineStatusIndicator
                                        property string currentServerId: chats.currentIndex >= 0 ? contactsModel.getServer(chats.currentIndex) : ""
                                        property string currentContactPubKey: chats.currentIndex >= 0 ? contactsModel.getPubKeyFingerprint(chats.currentIndex) : ""
                                        property bool currentIsOnline: false
                                        property bool currentServerConnected: false

                                        function refreshStatus() {
                                            currentServerId = chats.currentIndex >= 0 ? contactsModel.getServer(chats.currentIndex) : ""
                                            currentContactPubKey = chats.currentIndex >= 0 ? contactsModel.getPubKeyFingerprint(chats.currentIndex) : ""
                                            currentIsOnline = chats.currentIndex >= 0 ? contactsModel.getIsOnline(chats.currentIndex) : false
                                            currentServerConnected = currentServerId.length > 0 ? connections.isConnected(currentServerId) : false
                                        }

                                        visible: currentServerId.length > 0 && (currentIsOnline || !currentServerConnected)
                                        width: 14
                                        height: 14
                                        radius: 7
                                        antialiasing: true
                                        color: currentIsOnline ? "transparent" : theme.redAccent
                                        gradient: currentIsOnline ? headerOnlineDotGradient : null
                                        border.color: theme.background
                                        border.width: 2
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        Component.onCompleted: refreshStatus()
                                        Gradient {
                                            id: headerOnlineDotGradient
                                            orientation: Gradient.Horizontal
                                            GradientStop { position: 0.0; color: "#00c4dc" }
                                            GradientStop { position: 1.0; color: "#00ef96" }
                                        }
                                    }
                                    Connections { // !
                                        target: contactsModel
                                        function onContactUpdated(index) {
                                            if (index === chats.currentIndex
                                                    && onlineStatusIndicator.currentServerId === contactsModel.getServer(index)
                                                    && onlineStatusIndicator.currentContactPubKey === contactsModel.getPubKeyFingerprint(index)) {
                                                onlineStatusIndicator.refreshStatus()
                                            }
                                        }
                                    }
                                } // avatar
                                StyledName { // firstName + lastName
                                    id: additionalNameText
                                    firstName: contactsModel.getFirstName(chats.currentIndex)
                                    lastName: contactsModel.getLastName(chats.currentIndex)
                                    styleCss: contactsModel.getNameStyle(chats.currentIndex)
                                    pixelSize: 14
                                    fontFamily: "Roboto"
                                    fontWeight: Font.Medium
                                    fallbackColor: theme.accent
                                    Layout.topMargin: 10
                                    Layout.bottomMargin: 20
                                    Layout.alignment: Qt.AlignTop | Qt.AlignHCenter
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: implicitHeight
                                    Layout.rightMargin: 10
                                    Layout.leftMargin: 10
                                    elide: Text.ElideRight
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                }
                                RowLayout {
                                    id: aboutContainer
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 50
                                    Layout.topMargin: 30
                                    Layout.bottomMargin: 30
                                    Layout.alignment: Qt.AlignTop | Qt.AlignHCenter
                                    visible: chats.model.get(chats.currentIndex).aboutMe.length > 0
                                    Rectangle {
                                        Layout.alignment: Qt.AlignVCenter | Qt.AlignLeft
                                        Layout.preferredHeight: 50
                                        Layout.maximumHeight: 50
                                        Layout.minimumHeight: 50
                                        Layout.preferredWidth: 2
                                        Layout.maximumWidth: 2
                                        Layout.minimumWidth: 2
                                        color: "transparent"
                                        layer.enabled: true
                                        gradient: Gradient {
                                            GradientStop { position: 0.0; color: theme.background }
                                            GradientStop { position: 0.4; color: theme.lowAccent }
                                            GradientStop { position: 0.5; color: theme.accent }
                                            GradientStop { position: 0.6; color: theme.lowAccent }
                                            GradientStop { position: 1.0; color: theme.background }
                                            orientation: Gradient.Vertical
                                        }
                                    }
                                    Text { // about me
                                        Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                                        Layout.fillWidth: true
                                        id: aboutText
                                        text: contactsModel.getAboutMe(chats.currentIndex)
                                        font.pixelSize: 12
                                        font.family: "Roboto"
                                        font.weight: Font.Light
                                        color: theme.halfAccent
                                        Layout.leftMargin: 15
                                        Layout.rightMargin: 5
                                    }
                                    Connections { // !
                                        target: contactsModel
                                        function onContactUpdated(index) {
                                            if (index === chats.currentIndex) {
                                                aboutText.text = contactsModel.getAboutMe(chats.currentIndex)
                                                aboutContainer.visible = contactsModel.getAboutMe(chats.currentIndex).length > 0
                                            }
                                        }
                                    }
                                }
                                RightBarButton { // infoSecureButton
                                    id: infoSecureButton
                                    Layout.alignment: Qt.AlignTop
                                    Layout.fillWidth: true
                                    icon.source: "resources/secure.svg"
                                    buttonColor: theme.accent
                                    backgroundColor: theme.background
                                    text: qsTr("Chat is protected by end-to-end encryption")
                                    font.pixelSize: 12
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                    onClicked: {
                                        stack.push(e2eInfoPage)
                                    }
                                }
                                RightBarButton { // infoPubKeyButton
                                    id: infoPubKeyButton
                                    Layout.alignment: Qt.AlignTop
                                    Layout.fillWidth: true
                                    icon.source: "resources/fingerprint.svg"
                                    buttonColor: theme.accent
                                    backgroundColor: theme.background
                                    property string originalText: qsTr("Public key:\n") + contactsModel.getPubKeyFingerprint(chats.currentIndex)
                                    text: qsTr("Public key:\n") + contactsModel.getPubKeyFingerprint(chats.currentIndex) // ! move to a variable
                                    font.pixelSize: 12
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                    Timer {
                                        id: copiedTimer
                                        interval: 1000
                                        onTriggered: {
                                            infoPubKeyButton.text = infoPubKeyButton.originalText
                                        }
                                    }
                                    onClicked: {
                                        ClipboardHelper.setText(contactsModel.getPubKeyFingerprint(chats.currentIndex))
                                        text = qsTr("Public key copied to clipboard")
                                        copiedTimer.restart()
                                    }
                                }
                                RightBarButton { // infoSessionFingerprintButton
                                    id: infoSessionFingerprintButton

                                    property string sessionFingerprint: contactsModel.getSessionFingerprint(chats.currentIndex)

                                    Layout.alignment: Qt.AlignTop
                                    Layout.fillWidth: true
                                    icon.source: "resources/key.svg"
                                    buttonColor: theme.accent
                                    backgroundColor: theme.background
                                    text: qsTr("Session fingerprint:\n") + infoSessionFingerprintButton.sessionFingerprint
                                    font.pixelSize: 12
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                    onClicked: {
                                        stack.push(sessionFingerprintPage, {
                                            "fingerprint": infoSessionFingerprintButton.sessionFingerprint,
                                            "fromServer": contactsModel.getServer(chats.currentIndex),
                                            "fromPubKey" : contactsModel.getPubKeyFingerprint(chats.currentIndex)
                                        })
                                    }
                                }
                                Connections {
                                    target: MainSignals
                                    function onSessionFingerprintChanged(serverId, fromPubKey, newFingerprint) {
                                        if (serverId === contactsModel.getServer(chats.currentIndex)
                                                && fromPubKey === contactsModel.getPubKeyFingerprint(chats.currentIndex)) {
                                            infoSessionFingerprintButton.sessionFingerprint = newFingerprint
                                            //infoSessionFingerprintButton.text = qsTr("Session fingerprint:\n") + newFingerprint
                                        }
                                    }
                                }
                                // Connections { // !
                                //     target: contactsModel
                                //     function onContactUpdated(index) {
                                //         if (index === chats.currentIndex) {
                                //             infoSessionFingerprintButton.text = qsTr("Session fingerprint:\n") + contactsModel.getSessionFingerprint(chats.currentIndex)
                                //         }
                                //     }
                                // }
                                RightBarButton { // infoServerButton
                                    id: infoServerButton
                                    Layout.alignment: Qt.AlignTop
                                    Layout.fillWidth: true
                                    icon.source: "resources/server.svg"
                                    buttonColor: theme.accent
                                    backgroundColor: theme.background
                                    property string originalText: contactsModel.getServer(chats.currentIndex)
                                    text: contactsModel.getServer(chats.currentIndex)
                                    font.pixelSize: 12
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                    Timer {
                                        id: serverCopiedTimer
                                        interval: 1000
                                        onTriggered: {
                                            infoServerButton.text = infoServerButton.originalText
                                        }
                                    }
                                    onClicked: {
                                        ClipboardHelper.setText(contactsModel.getServer(chats.currentIndex))
                                        text = qsTr("Server address copied to clipboard")
                                        serverCopiedTimer.restart()
                                    }
                                }
                                Connections { // !
                                    target: contactsModel
                                    function onContactUpdated(index) {
                                        if (index === chats.currentIndex) {
                                            infoServerButton.originalText = contactsModel.getServer(chats.currentIndex)
                                            infoServerButton.text = contactsModel.getServer(chats.currentIndex)
                                        }
                                    }
                                }
                                // RightBarButton { // infoQrButton
                                //     id: infoQRButton
                                //     Layout.alignment: Qt.AlignTop
                                //     Layout.fillWidth: true
                                //     // Layout.preferredHeight: 40
                                //     icon.source: "resources/qr.svg"
                                //     buttonColor: theme.accent
                                //     backgroundColor: theme.background
                                //     text: "Share QR"
                                //     font.pixelSize: 12
                                //     font.family: geologicaFont.name
                                //     font.weight: Font.Light
                                //     onClicked: {
                                //         console.log("Share QR clicked")
                                //     }
                                // }
                                RightBarButton { // removeChatButton
                                    id: removeChatButton
                                    Layout.alignment: Qt.AlignTop
                                    Layout.fillWidth: true
                                    icon.source: "resources/remove.svg"
                                    buttonColor: theme.redAccent
                                    backgroundColor: theme.background
                                    text: qsTr("Remove chat")
                                    font.pixelSize: 12
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                    onClicked: {
                                        MainSignals.emitContactRemove(
                                            contactsModel.getServer(chats.currentIndex),
                                            contactsModel.getPubKeyFingerprint(chats.currentIndex)
                                        );
                                    }
                                }
                                RightBarButton { // removeChatButton
                                    id: clearChatButton
                                    Layout.alignment: Qt.AlignTop
                                    Layout.fillWidth: true
                                    icon.source: "resources/clear.svg"
                                    buttonColor: theme.redAccent
                                    backgroundColor: theme.background
                                    text: qsTr("Clear history")
                                    font.pixelSize: 12
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                    onClicked: {
                                        MainSignals.emitClearHistory(
                                            contactsModel.getServer(chats.currentIndex),
                                            contactsModel.getPubKeyFingerprint(chats.currentIndex)
                                        );
                                        if (mainPagePage.isMobileLayout)
                                            mainPagePage.showChatPane()
                                    }
                                }
                                RightBarButton { // donateRightButton
                                    id: donateRightButton
                                    Layout.topMargin: 10
                                    Layout.alignment: Qt.AlignTop
                                    Layout.fillWidth: true
                                    icon.source: "resources/donate.svg"
                                    buttonColor: theme.yellowAccent
                                    backgroundColor: theme.background
                                    text: qsTr("Donate")
                                    font.pixelSize: 12
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                    onClicked: {
                                        stack.push(donatePage)
                                    }
                                }
                                RightBarButton {
                                    id: updatesRightButton
                                    Layout.alignment: Qt.AlignTop
                                    Layout.fillWidth: true
                                    icon.source: "resources/update.svg"
                                    buttonColor: theme.accent
                                    backgroundColor: theme.background
                                    text: qsTr("Updates")
                                    font.pixelSize: 12
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                    onClicked: {
                                        stack.push(updatesPage)
                                    }
                                }
                                Rectangle { // filler
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "transparent"
                                }
                            } // AdditionalPanel
                        } // scrollArea
                    }
                }
                RoundButton { // FAB - Add contact floating action button
                    id: addContactFab
                    // Replaced by the floating bottomNavBar (which has an Add contact
                    // tab); kept hidden rather than removed to preserve desktop refs.
                    visible: false
                    anchors.bottom: parent.bottom
                    anchors.right: parent.right
                    anchors.bottomMargin: 35
                    anchors.rightMargin: 35
                    width: 60
                    height: 60
                    z: 100
                    icon.source: "resources/add_chat.svg"
                    icon.color: theme.background
                    icon.width: 25
                    icon.height: 25
                    Material.background: theme.accent
                    Material.elevation: 6
                    onClicked: stack.push(addContactPage)
                }
            }
        }
    }
    Component { // sessionFingerprintPage - page for displaying the current session fingerprint used to verify the contact
        id: sessionFingerprintPage
        Page {
            objectName: "sessionFingerprintPage"
            property string fingerprint: ""
            property string fromServer: ""
            property string fromPubKey: ""


            Connections {
                target: MainSignals
                function onSessionFingerprintChanged(serverId, fromPubKey2, newFingerprint) {
                    console.log('\n\n' + serverId + '\n' + fromServer + '\n' + fromPubKey2 + '\n' + fromPubKey + '\n\n')
                    if (serverId === fromServer && fromPubKey2 === fromPubKey) {
                        console.log('!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!')
                        fingerprint = newFingerprint
                        //infoSessionFingerprintButton.text = qsTr("Session fingerprint:\n") + newFingerprint
                    }
                }
            }

            Rectangle {
                anchors.fill: parent
                color: theme.background
                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }
                Column {
                    anchors.centerIn: parent
                    spacing: 10
                    Grid {
                        id: fingerprintGrid
                        columns: 10  // 5 columns (matching the number of fingerprint groups)
                        spacing: 2
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottomMargin: 40
                        Repeater {
                            model: fingerprintToColors(fingerprint)
                            Rectangle {
                                width: 20
                                height: 20
                                radius: 2
                                color: modelData
                            }
                        }
                    }
                    Text {
                        id: fingerprintText
                        property string originalText: fingerprint
                        text: fingerprint
                        anchors.horizontalCenter: parent.horizontalCenter
                        topPadding: 20
                        color: theme.accent
                        font.pixelSize: 16
                        font.family: geologicaFont.name
                        font.weight: Font.Bold
                        Timer {
                            id: copiedTimer
                            interval: 1000
                            onTriggered: {
                                fingerprintText.text = fingerprintText.originalText
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                ClipboardHelper.setText(fingerprint)
                                fingerprintText.text = qsTr("Session id copied to clipboard")
                                copiedTimer.restart()
                            }
                        }
                    }
                    Text {
                        text: qsTr("Use this fingerprint to verify the session with your contact.\nYou can compare it with the fingerprint shown on their device.")
                        anchors.horizontalCenter: parent.horizontalCenter
                        color: theme.lowAccent
                        topPadding: 60
                        font.pixelSize: Platform.isMobile ? 10 : 12
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }
            function fingerprintToColors(fp) {
                var colors = []
                var cleanFp = fp.replace(/-/g, "")
                var palette = [
                    theme.background,
                    theme.lowestAccent,
                    theme.lowAccent,
                    theme.halfAccent,
                    theme.accent,
                    theme.highAccent,
                    theme.highestAccent
                ]
                for (var i = 0; i < cleanFp.length; i++) {
                    var c = cleanFp.charAt(i)
                    var value
                    if (c >= '0' && c <= '9') {
                        value = parseInt(c)
                    } else if (c >= 'a' && c <= 'f') {
                        value = 10 + (c.charCodeAt(0) - 'a'.charCodeAt(0))
                    } else if (c >= 'A' && c <= 'F') {
                        value = 10 + (c.charCodeAt(0) - 'A'.charCodeAt(0))
                    } else {
                        value = 0
                    }
                    var highBits = Math.floor(value / 4)
                    var lowBits = value % 4
                    var xorValue = (value ^ i) % 7
                    var sumValue = (value + i) % 7
                    var invValue = (15 - value) % 7
                    colors.push(palette[Math.floor(highBits * 7 / 4)])
                    colors.push(palette[Math.floor(lowBits * 7 / 4)])
                    colors.push(palette[xorValue])
                    colors.push(palette[sumValue])
                    colors.push(palette[invValue])
                }
                return colors
            }
        }
    }
    Component { // settingsPage - page with account and application settings
        id: settingsPage
        Page {
            id: settingsPageRoot
            objectName: "settingsPage"

            // True while any account/server text field differs from what is
            // persisted. Bool toggles, theme and language save themselves the
            // instant they change, so they are deliberately not tracked here —
            // only the fields that wait for the Save button can be "unsaved".
            readonly property bool hasUnsavedChanges:
                   firstNameInput.inputText !== Settings.getTextSetting("firstName")
                || lastNameInput.inputText !== Settings.getTextSetting("lastName")
                || aboutMeInput.inputText !== Settings.getTextSetting("aboutMe")
                || nameStyleField.styleCss !== Settings.getTextSetting("nameStyle")
                || serverAddressInput.inputText !== Settings.getTextSetting("serverAddress")
                || updatesRepositoryInput.inputText !== Settings.getTextSetting("updatesRepository")

            // Navigation deferred until the user answers the unsaved-changes
            // dialog (set by requestLeaveConfirmation, run on Save / Don't save).
            property var pendingLeaveAction: null

            // Persist every settings field. Returns false and changes nothing
            // when the server address is invalid, so callers can keep the user
            // on the page to fix it.
            function saveAll() {
                // Flush any text still sitting in the IME composing region
                // (Android keyboards don't commit the last chars until a word
                // boundary, so "443" would be missing without this).
                Qt.inputMethod.commit()
                const cleanServer = mainWindow.sanitizeServerAddress(serverAddressInput.inputText)
                if (!mainWindow.isValidServerAddress(cleanServer))
                    return false
                Settings.setTextSetting("firstName", firstNameInput.inputText)
                Settings.setTextSetting("lastName", lastNameInput.inputText)
                Settings.setTextSetting("aboutMe", aboutMeInput.inputText)
                Settings.setTextSetting("nameStyle", nameStyleField.styleCss)
                Settings.setTextSetting("serverAddress", cleanServer)
                Settings.setTextSetting("updatesRepository", updatesRepositoryInput.inputText.trim())
                connections.addServer(cleanServer)
                connections.translateAccountInfo()
                return true
            }

            // Called by mainWindow.maybeGuardLeave() when navigation is attempted
            // with unsaved edits: remember where we were headed and ask the user.
            function requestLeaveConfirmation(proceed) {
                pendingLeaveAction = proceed
                unsavedChangesDialog.open()
            }

            // Back / Escape while the dialog is up cancels it (stay on the page).
            // Returns true if there was a dialog to dismiss.
            function dismissLeaveConfirmation() {
                if (!unsavedChangesDialog.opened)
                    return false
                unsavedChangesDialog.close()
                return true
            }

            Rectangle {

                BackButton {
                    id: backButton
                    visible: !Platform.isMobile
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: Platform.isMobile ? 0 : 20
                    anchors.leftMargin: Platform.isMobile ? 0 : 20
                    // Route the desktop back button through the unsaved-changes
                    // guard instead of popping straight away.
                    backAction: function() {
                        if (!mainWindow.maybeGuardLeave(function() { stack.pop() }))
                            stack.pop()
                    }
                }

                anchors.fill: parent
                color: theme.background

                ScrollView {
                    id: scrollArea1
                    WheelScroller { flickable: scrollArea1.contentItem }
                    width: parent.width * (Platform.isMobile ? 0.92 : 0.6)
                    contentWidth: parent.width * (Platform.isMobile ? 0.92 : 0.6)
                    anchors.horizontalCenter: parent.horizontalCenter
                    height: parent.height
                    // Trailing space so the last setting clears the floating bottom nav bar.
                    // Only while the pill is actually resting there — once the IME opens
                    // (edge-to-edge Android) the pill stays pinned under the keyboard instead
                    // of rising with it, so this reserve would just show as a bare strip of
                    // background above the keyboard.
                    bottomPadding: Platform.isMobile ? ((Platform.isAndroid && AndroidSystemUi.available && AndroidSystemUi.edgeToEdge && AndroidSystemUi.keyboardHeight > 0) ? 0 : 90) : 0
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AlwaysOff
                    }
                    ScrollBar.horizontal: ScrollBar {
                        policy: ScrollBar.AlwaysOff
                    }
                    ColumnLayout {
                        id: columnLayout
                        width: scrollArea1.width
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.topMargin: Platform.isMobile ? 0 : 40
                        anchors.bottomMargin: Platform.isMobile ? 0 : 40
                        spacing: 5
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Platform.isMobile ? 0 : 50
                            color: "transparent"
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Text {
                                text: qsTr("Settings")
                                font.pixelSize: 20
                                color: theme.accent
                                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                bottomPadding: Platform.isMobile ? 10 : 30
                                topPadding: Platform.isMobile ? 0 : 30
                                font.family: geologicaFont.name
                                font.weight: Font.Medium
                                Layout.fillWidth: true
                            }
                            // RightBarButton {
                            //     id: changePasswordButton
                            //     Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            //     icon.source: "resources/secure.svg"
                            //     buttonColor: theme.accent
                            //     backgroundColor: theme.background
                            //     text: qsTr("Password")
                            //     font.pixelSize: 12
                            //     font.family: geologicaFont.name
                            //     font.weight: Font.Light
                            //     onClicked: {
                            //         stack.push(changePasswordPage)
                            //     }
                            // }
                            RightBarButton {
                                id: saveSettingsButton2
                                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                icon.source: "resources/save.svg"
                                buttonColor: theme.accent
                                backgroundColor: theme.background
                                text: qsTr("Save")
                                font.pixelSize: 12
                                font.family: geologicaFont.name
                                font.weight: Font.Light
                                Timer {
                                    id: saveButtonResetTimer2
                                    interval: 1000
                                    running: true
                                    repeat: false
                                    onTriggered: {
                                        saveSettingsButton2.text = qsTr("Save")
                                    }
                                }
                                onClicked: {
                                    if (settingsPageRoot.saveAll()) {
                                        text = qsTr("Saved")
                                        saveButtonResetTimer2.restart()
                                        stack.pop()
                                    } else {
                                        text = qsTr("Invalid server address")
                                        saveButtonResetTimer2.restart()
                                    }
                                }
                            }
                            // Rectangle {
                            //     color: "transparent"
                            //     Layout.preferredWidth: settingsPage.compactLayout ? 0 : 20
                            //     Layout.maximumWidth: settingsPage.compactLayout ? 0 : 20
                            //     Layout.minimumWidth: settingsPage.compactLayout ? 0 : 20
                            //     Layout.fillHeight: true
                            // }
                        }
                        SettingsGroup {
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Account")
                            Layout.fillWidth: true
                        }
                        RightBarButton {
                            id: infoPubKeyButton
                            Layout.alignment: Qt.AlignTop
                            Layout.fillWidth: true
                            icon.source: "resources/fingerprint.svg"
                            buttonColor: theme.accent
                            backgroundColor: theme.background
                            property string originalText: qsTr("Public key:\n") + Account.getPublicKey()
                            text: qsTr("Public key:\n") + Account.getPublicKey()
                            font.pixelSize: Platform.isMobile ? 10 : 12
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            Timer {
                                id: copiedTimer
                                interval: 1000
                                onTriggered: {
                                    infoPubKeyButton.text = infoPubKeyButton.originalText
                                }
                            }
                            onClicked: {
                                ClipboardHelper.setText(Account.getPublicKey())
                                text = qsTr("Public key copied to clipboard")
                                copiedTimer.restart()
                            }
                        }
                        RowLayout {
                            Layout.topMargin: 15
                            Layout.bottomMargin: 15
                            Layout.fillWidth: true
                            Rectangle { // avatar
                                id: avatarContainer
                                property string avatar: Settings.getTextSetting("firstName").length > 0 ? Settings.getTextSetting("firstName").charAt(0).toUpperCase() : "A"
                                Layout.preferredWidth: 52
                                Layout.preferredHeight: 52
                                Layout.alignment: Qt.AlignVCenter
                                radius: width / 2
                                color: theme.minimumAccent
                                layer.enabled: true
                                Text {
                                    id: avatarLetter
                                    anchors.centerIn: parent
                                    text: avatarContainer.avatar
                                    font.pixelSize: 28
                                    font.family: geologicaFont.name
                                    color: theme.accent
                                    visible: Account.hasAvatar("0") === false
                                }
                                Image { // avatar from file
                                    id: avatarImg
                                    anchors.fill: parent
                                    visible: false
                                    source: "image://avatars/0"
                                    mipmap: true
                                    fillMode: Image.PreserveAspectCrop
                                    smooth: true
                                    antialiasing: true
                                    layer.enabled: true
                                    layer.smooth: true
                                }
                                Connections {
                                    target: Account
                                    function onAvatarChanged() {
                                        avatarImg.source = "image://avatars/0?t=" + Date.now()
                                        maskEffectImage.visible = Account.hasAvatar("0")
                                        avatarLetter.visible = !Account.hasAvatar("0")
                                    }
                                }
                                Rectangle { // avatar mask
                                    id: mask
                                    anchors.fill: parent
                                    radius: avatarContainer.radius
                                    visible: false
                                    layer.enabled: true
                                    layer.smooth: true
                                }
                                MultiEffect { // avatar circle effect
                                    id: maskEffectImage
                                    anchors.fill: parent
                                    source: avatarImg
                                    maskEnabled: true
                                    maskSource: mask
                                    visible: Account.hasAvatar("0")
                                    smooth: true
                                    antialiasing: true
                                    maskSpreadAtMin: 1.0
                                    maskThresholdMin: 0.6
                                }
                                Rectangle {
                                    visible: true
                                    width: 14
                                    height: 14
                                    radius: 7
                                    color: "#1bb141"
                                    border.color: theme.background
                                    border.width: 2
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        optionsMenu5.open()
                                    }
                                }
                            } // avatar
                            Menu {
                                id: optionsMenu5
                                y: parent.height
                                topInset: 0
                                bottomInset: 0
                                padding: 0
                                property bool leftAlign: false
                                property bool isRed: false
                                background: Rectangle {
                                    implicitWidth: 100
                                    color: mainWindow.chatPanelSurface
                                    border.color: mainWindow.chatPanelBorderColor
                                    border.width: 1
                                    radius: 14

                                    PanelBorder {
                                        edgeColor: mainWindow.chatPanelBorderColor
                                    }
                                }
                                MyMenuItem {
                                    text: qsTr("Delete")
                                    onTriggered: {
                                        console.log('Delete avatar clicked')
                                        MainSignals.emitAvatarDeleted()
                                        maskEffectImage.visible = false
                                        avatarLetter.visible = true
                                    }
                                    leftAlign: optionsMenu5.leftAlign
                                    isRed: true
                                    width: 100
                                    colorAccent: theme.accent
                                    colorBackground: theme.background
                                    colorHighlighted: mainWindow.adjustContrast(theme.background, 1.1)
                                    iconSource: "resources/remove.svg"
                                }
                            }
                            StyledName {
                                id: firstNameText
                                firstName: {
                                    var live = firstNameInput.inputText
                                    if (live && live.length > 0)
                                        return live
                                    // Fall back to the saved name so a transient empty
                                    // (IME quirk on focus) never flashes "Anonymous".
                                    var saved = Settings.getTextSetting("firstName")
                                    return (saved && saved.length > 0) ? saved : qsTr("Anonymous")
                                }
                                lastName: {
                                    var live = lastNameInput.inputText
                                    if (live && live.length > 0)
                                        return live
                                    return Settings.getTextSetting("lastName")
                                }
                                styleCss: Settings.getTextSetting("nameStyle")
                                pixelSize: 14
                                fontFamily: geologicaFont.name
                                fontWeight: Font.Light
                                fallbackColor: theme.accent
                                Layout.alignment: Qt.AlignVCenter | Qt.AlignLeft
                                Layout.leftMargin: 10
                                elide: Text.ElideRight
                                Layout.preferredWidth: Platform.isMobile ? 100 : 140
                            }
                            Rectangle { // spacer
                                color: "transparent"
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                            }
                            ActionButton {
                                id: changeAvatarButton
                                text: qsTr("Change Avatar")
                                buttonColor: theme.accent
                                textColor: theme.background
                                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                Layout.preferredHeight: 40
                                // Layout.preferredWidth: Platform.isMobile ? 112 : 140
                                font.family: geologicaFont.name
                                font.weight: Font.Light
                                font.pixelSize: Platform.isMobile ? 10 : 12
                                
                                onClicked: {
                                    Account.selectAndSaveAvatar()
                                }
                            }
                            Rectangle {
                                color: "transparent"
                                Layout.preferredWidth: Platform.isMobile ? 0 : 20
                                Layout.maximumWidth: Platform.isMobile ? 0 : 20
                                Layout.minimumWidth: Platform.isMobile ? 0 : 20
                                Layout.fillHeight: true
                            }
                        }
                        TextSetting {
                            id: firstNameInput
                            accentColor: theme.accent
                            lowAccentColor: theme.lowAccent
                            lowestAccentColor: theme.lowestAccent
                            backgroundColor: theme.background
                            settingText: qsTr("First Name")
                            settingName: "firstName"
                        }
                        TextSetting {
                            id: lastNameInput
                            accentColor: theme.accent
                            lowAccentColor: theme.lowAccent
                            lowestAccentColor: theme.lowestAccent
                            backgroundColor: theme.background
                            settingText: qsTr("Last Name")
                            settingName: "lastName"
                        }
                        ManyTextSetting {
                            id: aboutMeInput
                            accentColor: theme.accent
                            lowAccentColor: theme.lowAccent
                            lowestAccentColor: theme.lowestAccent
                            backgroundColor: theme.background
                            settingText: qsTr("About Me")
                            settingName: "aboutMe"
                        }
                        SettingsGroup {
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Unique name style")
                            Layout.fillWidth: true
                        }
                        NameStyleField {
                            id: nameStyleField
                            Layout.fillWidth: true
                            Layout.topMargin: 10
                            accentColor: theme.accent
                            lowAccentColor: theme.lowAccent
                            lowestAccentColor: theme.lowestAccent
                            backgroundColor: theme.background
                            previewFirstName: firstNameInput.inputText
                            previewLastName: lastNameInput.inputText
                        }
                        SettingsGroup {
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("General Settings")
                            Layout.fillWidth: true
                        }
                        // BoolSetting {
                        //     accentColor: theme.accent
                        //     settingText: qsTr("Start with system")
                        //     settingName: "startWithSystem"
                        // }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            LanguagesBox {
                                id: languageBox1

                                Layout.preferredHeight: 50
                                Layout.preferredWidth: settingsPage.compactLayout ? (settingsPage.settingsContentWidth - 10) / 2 : 140
                                Layout.fillWidth: settingsPage.compactLayout

                                Layout.topMargin: 10

                                transparentBool: false

                                backgroundColor: theme.background
                                accentColor: theme.accent
                                lowestAccentColor: mainWindow.adjustContrast(theme.accent, 1.7)
                                surfaceColor: mainWindow.chatPanelSurface
                                borderColor: mainWindow.chatPanelBorderColor
                            }

                            Item {
                                id: themeChangerBox

                                Layout.preferredHeight: 50
                                Layout.preferredWidth: settingsPage.compactLayout ? (settingsPage.settingsContentWidth - 10) / 2 : 140
                                Layout.fillWidth: settingsPage.compactLayout

                                Layout.topMargin: 10

                                property bool transparentBool: false

                                Rectangle {
                                    id: themeChangeButton
                                    anchors.fill: parent

                                    color: theme.background
                                    border.color: themeChangerBox.transparentBool ? "transparent" : theme.accent
                                    border.width: 1
                                    radius: 0

                                    Row {
                                        anchors.centerIn: parent
                                        spacing: 15

                                        ColorImage {
                                            source: "resources/brush.svg"
                                            width: 24
                                            height: 24
                                            accentColor: theme.accent
                                        }

                                        Text {
                                            text: qsTr("Theme")
                                            color: theme.accent
                                            font.pixelSize: 14
                                            font.family: geologicaFont.name
                                            verticalAlignment: Text.AlignVCenter
                                            anchors.verticalCenter: parent.verticalCenter
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            themesMenu.open()
                                        }
                                    }
                                }

                                Menu {
                                    id: themesMenu
                                    y: themeChangeButton.y + themeChangeButton.height
                                    x: themeChangeButton.x + (themeChangeButton.width - themesMenu.width) / 2
                                    topInset: 0
                                    bottomInset: 0
                                    padding: 0
                                    property bool leftAlign: true
                                    property bool isRed: false
                                    background: Rectangle {
                                        implicitWidth: 125
                                        color: mainWindow.chatPanelSurface
                                        border.color: mainWindow.chatPanelBorderColor
                                        border.width: 1
                                        radius: 14

                                        PanelBorder {
                                            edgeColor: mainWindow.chatPanelBorderColor
                                        }
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Dark"
                                        onTriggered: {
                                            setTheme("dark")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: telegraphTheme.accent
                                        colorBackground: telegraphTheme.background
                                        colorHighlighted: Qt.darker(telegraphTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Violet"
                                        onTriggered: {
                                            setTheme("violet")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: violetTheme.accent
                                        colorBackground: violetTheme.background
                                        colorHighlighted: Qt.darker(violetTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "OLED"
                                        onTriggered: {
                                            setTheme("highContrast")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: highContrastTheme.accent
                                        colorBackground: highContrastTheme.background
                                        colorHighlighted: Qt.darker(highContrastTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Amber"
                                        onTriggered: {
                                            setTheme("amber")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: amberTheme.accent
                                        colorBackground: amberTheme.background
                                        colorHighlighted: Qt.darker(amberTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Ocean"
                                        onTriggered: {
                                            setTheme("ocean")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: oceanTheme.accent
                                        colorBackground: oceanTheme.background
                                        colorHighlighted: Qt.darker(oceanTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Rose"
                                        onTriggered: {
                                            setTheme("rose")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: roseTheme.accent
                                        colorBackground: roseTheme.background
                                        colorHighlighted: Qt.darker(roseTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Terminal"
                                        onTriggered: {
                                            setTheme("terminal")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: darkTheme.accent
                                        colorBackground: darkTheme.background
                                        colorHighlighted: Qt.lighter(darkTheme.background, 1.4)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Night"
                                        onTriggered: {
                                            setTheme("night")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: nightTheme.accent
                                        colorBackground: nightTheme.background
                                        colorHighlighted: Qt.darker(nightTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Graphite"
                                        onTriggered: {
                                            setTheme("graphite")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: graphiteTheme.accent
                                        colorBackground: graphiteTheme.background
                                        colorHighlighted: Qt.darker(graphiteTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Indigo"
                                        onTriggered: {
                                            setTheme("indigo")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: indigoTheme.accent
                                        colorBackground: indigoTheme.background
                                        colorHighlighted: Qt.darker(indigoTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Coral"
                                        onTriggered: {
                                            setTheme("coral")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: coralTheme.accent
                                        colorBackground: coralTheme.background
                                        colorHighlighted: Qt.darker(coralTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Sage"
                                        onTriggered: {
                                            setTheme("sage")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: sageTheme.accent
                                        colorBackground: sageTheme.background
                                        colorHighlighted: Qt.darker(sageTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Copper"
                                        onTriggered: {
                                            setTheme("copper")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: copperTheme.accent
                                        colorBackground: copperTheme.background
                                        colorHighlighted: Qt.darker(copperTheme.background, 1.2)
                                    }
                                    MyMenuItemWithColorIcon {
                                        text: "Nord"
                                        onTriggered: {
                                            setTheme("nord")
                                        }
                                        leftAlign: languagesMenu.leftAlign
                                        isRed: false
                                        width: 125
                                        iconsource: "resources/brush.svg"
                                        iconColor: nordTheme.accent
                                        colorBackground: nordTheme.background
                                        colorHighlighted: Qt.darker(nordTheme.background, 1.2)
                                    }
                                }
                            }


                        }
                        SettingsGroup {
                            Layout.fillWidth: true
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Updates")
                        }

                        TextSetting {
                            id: updatesRepositoryInput
                            accentColor: theme.accent
                            lowAccentColor: theme.lowAccent
                            lowestAccentColor: theme.lowestAccent
                            backgroundColor: theme.background
                            settingText: qsTr("Repository")
                            settingName: "updatesRepository"
                        }

                        SettingsGroup {
                            Layout.fillWidth: true
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Notifications")
                        }
                        BoolSetting {
                            maximumContentWidth: settingsPage.settingsContentWidth
                            accentColor: theme.accent
                            settingText: qsTr("Enable Notifications")
                            settingName: "notificationsEnabled"
                        }
                        BoolSetting {
                            maximumContentWidth: settingsPage.settingsContentWidth
                            accentColor: theme.accent
                            settingText: qsTr("Enable sounds")
                            settingName: "soundsEnabled"
                        }
                        BoolSetting {
                            maximumContentWidth: settingsPage.settingsContentWidth
                            accentColor: theme.accent
                            settingText: qsTr("Enable sound from active chat")
                            settingName: "soundFromActiveChatEnabled"
                        }
                        SettingsGroup {
                            Layout.fillWidth: true
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Calls")
                        }
                        BoolSetting {
                            maximumContentWidth: settingsPage.settingsContentWidth
                            accentColor: theme.accent
                            settingText: qsTr("Enable sounds")
                            settingName: "callSoundsEnabled"
                            defaultValue: true
                        }
                        BoolSetting {
                            visible: AndroidSystemUi.available
                            maximumContentWidth: settingsPage.settingsContentWidth
                            accentColor: theme.accent
                            settingText: qsTr("Enable vibration")
                            settingName: "callVibrationEnabled"
                            defaultValue: true
                        }

                        SettingsGroup {
                            Layout.fillWidth: true
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Media")
                        }
                        BoolSetting {
                            maximumContentWidth: settingsPage.settingsContentWidth
                            accentColor: theme.accent
                            settingText: qsTr("Live media-preview")
                            settingName: "liveMediaPreview"
                            defaultValue: true
                        }
                        SettingsGroup {
                            Layout.fillWidth: true
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Auto-download")
                        }
                        BoolSetting {
                            maximumContentWidth: settingsPage.settingsContentWidth
                            accentColor: theme.accent
                            settingText: qsTr("Photos and videos")
                            settingName: "autoDownloadMedia"
                            defaultValue: true
                        }
                        BoolSetting {
                            maximumContentWidth: settingsPage.settingsContentWidth
                            accentColor: theme.accent
                            settingText: qsTr("Files")
                            settingName: "autoDownloadFiles"
                            defaultValue: false
                        }
                        BoolSetting {
                            maximumContentWidth: settingsPage.settingsContentWidth
                            accentColor: theme.accent
                            settingText: qsTr("Voice messages")
                            settingName: "autoDownloadVoice"
                            defaultValue: true
                        }
                        SettingsGroup {
                            Layout.fillWidth: true
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Audio")
                        }
                        RowLayout {
                            Layout.preferredWidth: settingsPage.settingsContentWidth
                            Layout.maximumWidth: settingsPage.settingsContentWidth
                            Layout.minimumWidth: settingsPage.settingsContentWidth
                            Layout.preferredHeight: 44
                            Layout.topMargin: 10

                            Text {
                                text: qsTr("Microphone")
                                font.pixelSize: 14
                                color: theme.accent
                                Layout.alignment: Qt.AlignLeft
                                font.family: geologicaFont.name
                                font.weight: Font.Light
                                rightPadding: 10
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 34
                                color: "transparent"

                                ComboBox {
                                    id: audioInputComboBox
                                    anchors.fill: parent
                                    model: audioDeviceManager.inputDevices
                                    textRole: "description"
                                    currentIndex: audioDeviceManager.selectedInputIndex

                                    onActivated: function(index) {
                                        audioDeviceManager.setSelectedInputIndex(index)
                                    }

                                    contentItem: Text {
                                        leftPadding: 10
                                        rightPadding: audioInputComboBox.indicator.width + audioInputComboBox.spacing
                                        text: audioInputComboBox.displayText
                                        font.pixelSize: 13
                                        font.family: geologicaFont.name
                                        font.weight: Font.Light
                                        color: theme.accent
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }

                                    indicator: ColorImage {
                                        x: audioInputComboBox.width - width - 10
                                        y: (audioInputComboBox.height - height) / 2
                                        width: 14
                                        height: 14
                                        source: "resources/arrow_left.svg"
                                        rotation: -90
                                        accentColor: theme.accent
                                    }

                                    background: Rectangle {
                                        color: "transparent"
                                        border.color: theme.lowAccent
                                        radius: 0
                                    }

                                    popup: Popup {
                                        y: audioInputComboBox.height
                                        width: audioInputComboBox.width
                                        padding: 0
                                        background: Rectangle {
                                            color: mainWindow.chatPanelSurface
                                            border.color: mainWindow.chatPanelBorderColor
                                            border.width: 1
                                            radius: 14

                                            PanelBorder {
                                                edgeColor: mainWindow.chatPanelBorderColor
                                            }
                                        }

                                        contentItem: ListView {
                                            clip: true
                                            implicitHeight: contentHeight
                                            model: audioInputComboBox.popup.visible ? audioInputComboBox.delegateModel : null
                                            currentIndex: audioInputComboBox.highlightedIndex
                                        }
                                    }

                                    delegate: ItemDelegate {
                                        required property int index
                                        required property var model

                                        width: audioInputComboBox.width
                                        padding: 10

                                        background: Rectangle {
                                            // Transparent base reveals the popup's elevated surface; hover
                                            // brightens it ~3% via a theme-agnostic white overlay.
                                            color: highlighted ? Qt.rgba(1, 1, 1, 0.03) : "transparent"
                                            radius: 8
                                            anchors.fill: parent
                                            anchors.margins: 2
                                        }

                                        contentItem: Text {
                                            text: model.description
                                            font.pixelSize: 13
                                            font.family: geologicaFont.name
                                            font.weight: Font.Light
                                            color: theme.accent
                                            elide: Text.ElideRight
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                color: "transparent"
                                Layout.preferredWidth: settingsPage.compactLayout ? 0 : 20
                                Layout.maximumWidth: settingsPage.compactLayout ? 0 : 20
                                Layout.minimumWidth: settingsPage.compactLayout ? 0 : 20
                                Layout.fillHeight: true
                            }
                        }
                        SettingsGroup {
                            Layout.fillWidth: true
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("For advanced users")
                        }
                        TextSetting {
                            id: serverAddressInput
                            accentColor: theme.accent
                            lowAccentColor: theme.lowAccent
                            lowestAccentColor: theme.lowestAccent
                            backgroundColor: theme.background
                            settingText: qsTr("Server Address")
                            settingName: "serverAddress"
                        }
                        SettingsGroup {
                            Layout.fillWidth: true
                            accentColor: theme.redAccent
                            textColor: theme.redAccent
                            groupText: qsTr("Security")
                        }
                        RightBarButton {
                            id: hackedButton
                            Layout.alignment: Qt.AlignTop
                            Layout.fillWidth: true
                            icon.source: "resources/hacker.svg"
                            buttonColor: theme.redAccent
                            backgroundColor: theme.background
                            text: qsTr("I've been hacked")
                            font.pixelSize: Platform.isMobile ? 12 : 14
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            onClicked: stack.push(hackedPage)
                        }
                        RightBarButton {
                            id: resetAccountButton
                            Layout.alignment: Qt.AlignTop
                            Layout.fillWidth: true
                            icon.source: "resources/remove.svg"
                            buttonColor: theme.redAccent
                            backgroundColor: theme.background
                            text: qsTr("Reset app & delete account")
                            font.pixelSize: Platform.isMobile ? 12 : 14
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            onClicked: stack.push(resetPage)
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 30
                            color: "transparent"
                        }
                    }
                }
            }

            // Unsaved-changes confirmation. Shown when the user tries to leave
            // the settings page (back button, back gesture, or a bottom-nav tab)
            // with account/server edits that were never saved.
            Popup {
                id: unsavedChangesDialog
                modal: true
                focus: true
                dim: true
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                parent: Overlay.overlay
                anchors.centerIn: Overlay.overlay
                padding: 0

                // On a phone (or a very narrow desktop window) the buttons stack
                // vertically and the card hugs the screen width; otherwise it is
                // a compact centred card with the buttons in a row.
                readonly property bool compact: Platform.isMobile || mainWindow.width < 560
                width: compact ? Math.min(mainWindow.width - 48, 420) : 420

                Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.55) }
                background: Item {}

                // Reset transient state whenever it opens/closes so a stale error
                // never lingers and a dismissed dialog cancels the pending move.
                onOpened: dialogError.text = ""
                onClosed: settingsPageRoot.pendingLeaveAction = null

                contentItem: Rectangle {
                    id: dialogCard
                    implicitWidth: unsavedChangesDialog.width
                    implicitHeight: dialogColumn.implicitHeight + 48
                    radius: 16
                    color: theme.background
                    border.width: 1
                    border.color: theme.lowestAccent

                    ColumnLayout {
                        id: dialogColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 24
                        spacing: 14

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            Rectangle {
                                Layout.preferredWidth: 40
                                Layout.preferredHeight: 40
                                Layout.alignment: Qt.AlignTop
                                radius: width / 2
                                color: theme.minimumAccent
                                ColorImage {
                                    anchors.centerIn: parent
                                    width: 20
                                    height: 20
                                    source: "resources/save.svg"
                                    accentColor: theme.accent
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                spacing: 6

                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("Save changes?")
                                    color: theme.accent
                                    font.pixelSize: 18
                                    font.family: geologicaFont.name
                                    font.weight: Font.Medium
                                    wrapMode: Text.WordWrap
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("You have unsaved changes. Do you want to save them before leaving?")
                                    color: theme.halfAccent
                                    font.pixelSize: 13
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                    wrapMode: Text.WordWrap
                                    lineHeight: 1.2
                                }
                            }
                        }

                        Text {
                            id: dialogError
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: ""
                            color: theme.redAccent
                            font.pixelSize: 12
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            wrapMode: Text.WordWrap
                        }

                        GridLayout {
                            Layout.fillWidth: unsavedChangesDialog.compact
                            Layout.alignment: unsavedChangesDialog.compact ? Qt.AlignLeft : Qt.AlignRight
                            Layout.topMargin: 4
                            columns: unsavedChangesDialog.compact ? 1 : 3
                            rowSpacing: 8
                            columnSpacing: 8

                            // Cancel — stay on the page, discard nothing.
                            Rectangle {
                                Layout.fillWidth: unsavedChangesDialog.compact
                                implicitWidth: Math.max(104, cancelLabel.implicitWidth + 34)
                                implicitHeight: 42
                                radius: 8
                                color: cancelHover.containsMouse
                                       ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.08)
                                       : "transparent"
                                border.width: 1
                                border.color: theme.lowestAccent
                                Behavior on color { ColorAnimation { duration: 110 } }
                                Text {
                                    id: cancelLabel
                                    anchors.centerIn: parent
                                    text: qsTr("Cancel")
                                    color: theme.accent
                                    font.pixelSize: 13
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                }
                                MouseArea {
                                    id: cancelHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: unsavedChangesDialog.close()
                                }
                            }

                            // Don't save — discard the edits and leave.
                            Rectangle {
                                Layout.fillWidth: unsavedChangesDialog.compact
                                implicitWidth: Math.max(104, discardLabel.implicitWidth + 34)
                                implicitHeight: 42
                                radius: 8
                                color: discardHover.containsMouse
                                       ? Qt.rgba(theme.redAccent.r, theme.redAccent.g, theme.redAccent.b, 0.10)
                                       : "transparent"
                                border.width: 1
                                border.color: theme.redAccent
                                Behavior on color { ColorAnimation { duration: 110 } }
                                Text {
                                    id: discardLabel
                                    anchors.centerIn: parent
                                    text: qsTr("Don't save")
                                    color: theme.redAccent
                                    font.pixelSize: 13
                                    font.family: geologicaFont.name
                                    font.weight: Font.Light
                                }
                                MouseArea {
                                    id: discardHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        var act = settingsPageRoot.pendingLeaveAction
                                        unsavedChangesDialog.close()
                                        if (act) act()
                                    }
                                }
                            }

                            // Save — persist, then continue where we were headed.
                            Rectangle {
                                Layout.fillWidth: unsavedChangesDialog.compact
                                implicitWidth: Math.max(104, saveLabel.implicitWidth + 34)
                                implicitHeight: 42
                                radius: 8
                                color: saveHover.containsMouse ? Qt.darker(theme.accent, 1.12) : theme.accent
                                Behavior on color { ColorAnimation { duration: 110 } }
                                Text {
                                    id: saveLabel
                                    anchors.centerIn: parent
                                    text: qsTr("Save")
                                    color: theme.background
                                    font.pixelSize: 13
                                    font.family: geologicaFont.name
                                    font.weight: Font.Medium
                                }
                                MouseArea {
                                    id: saveHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (settingsPageRoot.saveAll()) {
                                            var act = settingsPageRoot.pendingLeaveAction
                                            unsavedChangesDialog.close()
                                            if (act) act()
                                        } else {
                                            dialogError.text = qsTr("Server address is invalid. Fix it before saving.")
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    Component { // hackedPage - "I've been hacked" panic screen: revoke the identity key after a code confirmation
        id: hackedPage
        Page {
            id: hackedPagePage
            objectName: "hackedPage"

            // Anti-fat-finger confirmation code. It only guards against an accidental tap
            // (it is shown on the same screen the attacker would also see), not against a
            // real attacker - see the threat model in JOBS.md #10. Regenerated on open.
            property string confirmCode: ""
            // Persisted once the user revokes their own key, so re-opening this page
            // shows the "already revoked" state instead of letting them run the flow
            // again. The server treats revocation as idempotent regardless; this is
            // the local guard against a pointless second attempt.
            property bool alreadyRevoked: Settings.getBoolSetting("ownIdentityRevoked", false)

            function generateCode() {
                var s = ""
                for (var i = 0; i < 16; ++i)
                    s += Math.floor(Math.random() * 10)
                confirmCode = s
            }

            Component.onCompleted: generateCode()

            Rectangle {
                anchors.fill: parent
                color: theme.background

                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }

                ScrollView {
                    id: hackedScroll
                    anchors.top: parent.top
                    anchors.topMargin: 80
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width * (Platform.isMobile ? 0.86 : 0.5)
                    ScrollBar.vertical.policy: ScrollBar.AsNeeded
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        id: hackedColumn
                        width: hackedScroll.width
                        spacing: 16

                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 12

                            ColorImage {
                                source: "resources/hacker.svg"
                                accentColor: theme.redAccent
                                Layout.preferredWidth: 30
                                Layout.preferredHeight: 30
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Text {
                                text: qsTr("I've been hacked")
                                color: theme.redAccent
                                font.pixelSize: 22
                                font.family: geologicaFont.name
                                font.weight: Font.Bold
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }

                        GradientLine {
                            Layout.fillWidth: true
                            height: 2
                            colorSide: "transparent"
                            colorOutSide: theme.redAccent
                            colorCenter: theme.redAccent
                        }

                        Text {
                            visible: !hackedPagePage.alreadyRevoked
                            text: qsTr("This permanently revokes your key. Everyone you have chatted with will stop trusting it, all your sessions will be terminated, and no one will be able to start a new session with this identity.<br><br><b>This cannot be undone.</b> Afterwards you will have to create a new identity and re-verify it in person with every contact.")
                            color: theme.accent
                            font.pixelSize: 14
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            textFormat: Text.RichText
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Text {
                            visible: !hackedPagePage.alreadyRevoked
                            text: qsTr("Type this code to confirm:")
                            color: theme.halfAccent
                            font.pixelSize: 13
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            Layout.topMargin: 6
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Text {
                            visible: !hackedPagePage.alreadyRevoked
                            text: hackedPagePage.confirmCode.replace(/(.{4})(?=.)/g, "$1 ")
                            color: theme.redAccent
                            font.pixelSize: 18
                            font.family: geologicaFont.name
                            font.weight: Font.Bold
                            font.letterSpacing: 2
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WrapAnywhere
                        }

                        TextField {
                            id: codeField
                            visible: !hackedPagePage.alreadyRevoked
                            // The committed text plus the in-progress IME preedit, with any
                            // non-digits stripped. On Android the last typed digit often sits
                            // in the composing buffer (preeditText) and is not yet in `text`,
                            // which made the confirm button stay dim for a code that looked
                            // correct; stray spaces are dropped too.
                            readonly property string enteredCode: (text + preeditText).replace(/[^0-9]/g, "")
                            Layout.preferredWidth: 300
                            Layout.maximumWidth: hackedColumn.width
                            Layout.alignment: Qt.AlignHCenter
                            horizontalAlignment: TextInput.AlignHCenter
                            maximumLength: 16
                            inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhNoPredictiveText
                            validator: RegularExpressionValidator { regularExpression: /[0-9]{0,16}/ }
                            font.pixelSize: 16
                            font.family: geologicaFont.name
                            font.letterSpacing: 2
                            color: theme.accent
                            selectedTextColor: theme.background
                            // Bring up the on-screen keyboard on mobile when the field gains
                            // focus (the app manages the Android IME manually).
                            onActiveFocusChanged: {
                                if (activeFocus && Platform.isAndroid && AndroidSystemUi.available)
                                    Qt.callLater(function() {
                                        if (codeField.activeFocus) {
                                            mainWindow.lastKeyboardTarget = codeField
                                            AndroidSystemUi.showKeyboardWithHints(codeField.inputMethodHints)
                                        }
                                    })
                            }
                            // Re-tapping an already-focused field does not change activeFocus,
                            // so the focus handler above would not re-open a manually dismissed
                            // keyboard. This re-requests it on every tap.
                            TapHandler {
                                enabled: Platform.isAndroid && AndroidSystemUi.available
                                onTapped: {
                                    codeField.forceActiveFocus()
                                    mainWindow.lastKeyboardTarget = codeField
                                    AndroidSystemUi.showKeyboardWithHints(codeField.inputMethodHints)
                                }
                            }
                            background: Rectangle {
                                color: theme.lowestAccent
                                radius: 4
                                border.width: 1
                                border.color: codeField.enteredCode.length === 16
                                    ? (codeField.enteredCode === hackedPagePage.confirmCode ? theme.redAccent : theme.lowAccent)
                                    : "transparent"
                            }
                        }

                        ActionButton {
                            id: confirmHackedButton
                            text: qsTr("Revoke my key")
                            buttonColor: theme.redAccent
                            textColor: theme.background
                            font.family: geologicaFont.name
                            font.weight: Font.Medium
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 6
                            visible: !hackedPagePage.alreadyRevoked
                            enabled: codeField.enteredCode === hackedPagePage.confirmCode && !hackedPagePage.alreadyRevoked
                            opacity: enabled ? 1.0 : 0.4
                            onClicked: {
                                // Signs the revocation certificate with the identity key and publishes
                                // it: tombstone to every connected server (blocks new sessions + drops
                                // our prekeys) and a signed in-band notice to every contact. Terminal
                                // and irreversible; recovery means a brand-new identity re-verified in
                                // person. See handleRevokeIdentity on the server.
                                Settings.setBoolSetting("ownIdentityRevoked", true)
                                hackedPagePage.alreadyRevoked = true
                                connections.revokeIdentity()
                            }
                        }

                        Text {
                            id: statusText
                            visible: hackedPagePage.alreadyRevoked
                            text: qsTr("Your key is already revoked. It can no longer be used, and your contacts have been notified. Create a new identity and re-verify it in person with each contact.")
                            color: theme.redAccent
                            font.pixelSize: 13
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 6
                        }

                        // Bottom breathing room so the button/status are reachable above the
                        // keyboard when the view scrolls.
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 40
                        }
                    }
                }

                // Keep the focused field visible above the Android keyboard: the StackView
                // already shrinks by the keyboard height, so scroll the field into the
                // remaining area (same approach as the settings page).
                Connections {
                    target: Platform.isAndroid && AndroidSystemUi.available ? AndroidSystemUi : null
                    function onKeyboardHeightChanged() {
                        if (AndroidSystemUi.keyboardHeight <= 0)
                            return
                        var focusItem = mainWindow.activeFocusItem || mainWindow.lastKeyboardTarget
                        if (!focusItem)
                            return
                        var pos = focusItem.mapToItem(hackedColumn, 0, 0)
                        if (pos.y < 0 || pos.y >= hackedColumn.height)
                            return
                        var itemBottom = pos.y + focusItem.height
                        var visibleBottom = hackedScroll.contentItem.contentY + hackedScroll.height
                        if (itemBottom > visibleBottom)
                            hackedScroll.contentItem.contentY = Math.max(0, itemBottom - hackedScroll.height + 20)
                    }
                }
            }
        }
    }
    Component { // resetPage - factory-reset / delete-account screen, confirmed with a 16-digit code
        id: resetPage
        Page {
            id: resetPagePage
            objectName: "resetPage"

            // Anti-fat-finger confirmation code, regenerated on open (same idea as
            // the hacked page). Guards a permanent, irreversible wipe.
            property string confirmCode: ""
            // Whether to also ask every contact to clear their copy first.
            property bool clearRemote: false
            // Latches once the wipe starts so the button can't be pressed twice.
            property bool resetting: false

            function generateCode() {
                var s = ""
                for (var i = 0; i < 16; ++i)
                    s += Math.floor(Math.random() * 10)
                confirmCode = s
            }

            Component.onCompleted: generateCode()

            Rectangle {
                anchors.fill: parent
                color: theme.background

                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }

                ScrollView {
                    id: resetScroll
                    anchors.top: parent.top
                    anchors.topMargin: 80
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width * (Platform.isMobile ? 0.86 : 0.5)
                    ScrollBar.vertical.policy: ScrollBar.AsNeeded
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        id: resetColumn
                        width: resetScroll.width
                        spacing: 16

                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 12

                            ColorImage {
                                source: "resources/remove.svg"
                                accentColor: theme.redAccent
                                Layout.preferredWidth: 28
                                Layout.preferredHeight: 28
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Text {
                                text: qsTr("Reset & delete account")
                                color: theme.redAccent
                                font.pixelSize: 22
                                font.family: geologicaFont.name
                                font.weight: Font.Bold
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }

                        GradientLine {
                            Layout.fillWidth: true
                            height: 2
                            colorSide: "transparent"
                            colorOutSide: theme.redAccent
                            colorCenter: theme.redAccent
                        }

                        Text {
                            visible: !resetPagePage.resetting
                            text: qsTr("This erases everything on this device — all conversations, all contacts, your account and every setting.<br><br><b>This cannot be undone.</b>")
                            color: theme.accent
                            font.pixelSize: 14
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            textFormat: Text.RichText
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        RowLayout { // in-window checkbox
                            visible: !resetPagePage.resetting
                            Layout.fillWidth: true
                            Layout.topMargin: 2
                            spacing: 10

                            Rectangle {
                                Layout.preferredWidth: 22
                                Layout.preferredHeight: 22
                                Layout.alignment: Qt.AlignVCenter
                                radius: 4
                                color: resetPagePage.clearRemote ? theme.redAccent : "transparent"
                                border.width: 1
                                border.color: theme.redAccent
                                ColorImage {
                                    anchors.centerIn: parent
                                    visible: resetPagePage.clearRemote
                                    source: "resources/checked.svg"
                                    accentColor: theme.background
                                    width: 14
                                    height: 14
                                }
                            }
                            Text {
                                text: qsTr("Clear all conversations at your contacts first")
                                color: theme.accent
                                font.pixelSize: 13
                                font.family: geologicaFont.name
                                font.weight: Font.Light
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                            }
                            TapHandler {
                                onTapped: resetPagePage.clearRemote = !resetPagePage.clearRemote
                            }
                        }

                        Text {
                            visible: !resetPagePage.resetting
                            text: qsTr("Type this code to confirm:")
                            color: theme.halfAccent
                            font.pixelSize: 13
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            Layout.topMargin: 6
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Text {
                            visible: !resetPagePage.resetting
                            text: resetPagePage.confirmCode.replace(/(.{4})(?=.)/g, "$1 ")
                            color: theme.redAccent
                            font.pixelSize: 18
                            font.family: geologicaFont.name
                            font.weight: Font.Bold
                            font.letterSpacing: 2
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WrapAnywhere
                        }

                        TextField {
                            id: resetCodeField
                            visible: !resetPagePage.resetting
                            // text + in-progress IME preedit, non-digits stripped (Android often
                            // keeps the last digit in the composing buffer, out of `text`).
                            readonly property string enteredCode: (text + preeditText).replace(/[^0-9]/g, "")
                            Layout.preferredWidth: 300
                            Layout.maximumWidth: resetColumn.width
                            Layout.alignment: Qt.AlignHCenter
                            horizontalAlignment: TextInput.AlignHCenter
                            maximumLength: 16
                            inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhNoPredictiveText
                            validator: RegularExpressionValidator { regularExpression: /[0-9]{0,16}/ }
                            font.pixelSize: 16
                            font.family: geologicaFont.name
                            font.letterSpacing: 2
                            color: theme.accent
                            selectedTextColor: theme.background
                            onActiveFocusChanged: {
                                if (activeFocus && Platform.isAndroid && AndroidSystemUi.available)
                                    Qt.callLater(function() {
                                        if (resetCodeField.activeFocus) {
                                            mainWindow.lastKeyboardTarget = resetCodeField
                                            AndroidSystemUi.showKeyboardWithHints(resetCodeField.inputMethodHints)
                                        }
                                    })
                            }
                            TapHandler {
                                enabled: Platform.isAndroid && AndroidSystemUi.available
                                onTapped: {
                                    resetCodeField.forceActiveFocus()
                                    mainWindow.lastKeyboardTarget = resetCodeField
                                    AndroidSystemUi.showKeyboardWithHints(resetCodeField.inputMethodHints)
                                }
                            }
                            background: Rectangle {
                                color: theme.lowestAccent
                                radius: 4
                                border.width: 1
                                border.color: resetCodeField.enteredCode.length === 16
                                    ? (resetCodeField.enteredCode === resetPagePage.confirmCode ? theme.redAccent : theme.lowAccent)
                                    : "transparent"
                            }
                        }

                        ActionButton {
                            id: resetConfirmButton
                            text: qsTr("Erase everything")
                            buttonColor: theme.redAccent
                            textColor: theme.background
                            font.family: geologicaFont.name
                            font.weight: Font.Medium
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 6
                            visible: !resetPagePage.resetting
                            enabled: resetCodeField.enteredCode === resetPagePage.confirmCode && !resetPagePage.resetting
                            opacity: enabled ? 1.0 : 0.4
                            onClicked: {
                                // Optionally clears conversations at every contact (e2e), then wipes
                                // the local database and returns to onboarding. Irreversible.
                                resetPagePage.resetting = true
                                mainWindow.performAccountReset(resetPagePage.clearRemote)
                            }
                        }

                        Text {
                            id: resetStatusText
                            visible: resetPagePage.resetting
                            text: qsTr("Erasing everything…")
                            color: theme.redAccent
                            font.pixelSize: 13
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 6
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 40
                        }
                    }
                }

                Connections {
                    target: Platform.isAndroid && AndroidSystemUi.available ? AndroidSystemUi : null
                    function onKeyboardHeightChanged() {
                        if (AndroidSystemUi.keyboardHeight <= 0)
                            return
                        var focusItem = mainWindow.activeFocusItem || mainWindow.lastKeyboardTarget
                        if (!focusItem)
                            return
                        var pos = focusItem.mapToItem(resetColumn, 0, 0)
                        if (pos.y < 0 || pos.y >= resetColumn.height)
                            return
                        var itemBottom = pos.y + focusItem.height
                        var visibleBottom = resetScroll.contentItem.contentY + resetScroll.height
                        if (itemBottom > visibleBottom)
                            resetScroll.contentItem.contentY = Math.max(0, itemBottom - resetScroll.height + 20)
                    }
                }
            }
        }
    }
    Component { // messageStasusInfoPage
        id: messageStatusInfoPage
        Page {
            objectName: "messageStatusInfoPage"
            Rectangle {
                anchors.fill: parent
                color: theme.background
                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }
                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 30
                    RowLayout {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 10
                        ColorImage {
                            source: "resources/sending.svg"
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 20

                            accentColor: theme.accent

                            RotationAnimator on rotation {
                                from: 0
                                to: 360
                                duration: 800
                                running: true
                                loops: Animation.Infinite
                            }
                        }
                        Text {
                            text: qsTr("Message sending to server")
                            color: theme.accent
                            font.pixelSize: 16
                            font.family: geologicaFont.name
                            font.weight: Font.Light                            
                        }
                    }
                    RowLayout {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 10
                        ColorImage {
                            source: "resources/internet.svg"
                            Layout.preferredWidth: 20
                            accentColor: theme.accent
                            Layout.preferredHeight: 20
                        }
                        Text {
                            text: qsTr("Message sent to server but not yet delivered")
                            color: theme.accent
                            font.pixelSize: 16
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                        }
                    }
                    RowLayout {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 10
                        ColorImage {
                            source: "resources/checked.svg"
                            Layout.preferredWidth: 20
                            accentColor: theme.accent
                            Layout.preferredHeight: 20
                        }
                        Text {
                            text: qsTr("Message delivered to recipient")
                            color: theme.accent
                            font.pixelSize: 16
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                        }
                    }
                    RowLayout {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 10
                        ColorImage {
                            source: "resources/double_checked.svg"
                            Layout.preferredWidth: 20
                            accentColor: theme.accent
                            Layout.preferredHeight: 20
                        }
                        Text {
                            text: qsTr("Message read by recipient")
                            color: theme.accent
                            font.pixelSize: 16
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                        }
                    }
                }
            }
        }
    }
    Component { // e2eInfoPage - page explaining end-to-end encryption and how it works in this messenger
        id: e2eInfoPage
        Page {
            objectName: "e2eInfoPage"
            readonly property bool compactLayout: width < 640 || Platform.isMobile
            Rectangle {
                anchors.fill: parent
                color: theme.background
                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }
                ScrollView {
                    id: e2eInfoScroll
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.topMargin: compactLayout ? 72 : 40
                    anchors.leftMargin: compactLayout ? 12 : 20
                    anchors.rightMargin: compactLayout ? 12 : 20
                    anchors.bottomMargin: compactLayout ? 12 : 20
                    contentWidth: availableWidth
                    contentHeight: e2eInfoLayout.implicitHeight
                    clip: true
                    WheelScroller { flickable: e2eInfoScroll.contentItem }
                    ScrollBar.horizontal: null

                    ColumnLayout {
                        id: e2eInfoLayout
                        width: Math.min(compactLayout ? parent.width : 500, parent.width)
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: compactLayout ? 14 : 10

                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: compactLayout ? 96 : 70

                            RowLayout {
                                anchors.centerIn: parent
                                spacing: compactLayout ? 10 : 25

                                ColorImage {
                                    source: "resources/notebook.svg"
                                    Layout.preferredHeight: compactLayout ? 44 : 70
                                    Layout.preferredWidth: compactLayout ? 44 : 70
                                    accentColor: theme.accent
                                }
                                ColorImage {
                                    source: "resources/arrows.svg"
                                    Layout.preferredHeight: compactLayout ? 28 : 50
                                    Layout.preferredWidth: compactLayout ? 28 : 50
                                    accentColor: theme.accent
                                }
                                ColorImage {
                                    source: "resources/blowfish.svg"
                                    Layout.preferredHeight: compactLayout ? 44 : 70
                                    Layout.preferredWidth: compactLayout ? 44 : 70
                                    accentColor: theme.accent
                                }
                                ColorImage {
                                    source: "resources/arrows.svg"
                                    Layout.preferredHeight: compactLayout ? 28 : 50
                                    Layout.preferredWidth: compactLayout ? 28 : 50
                                    accentColor: theme.accent
                                }
                                ColorImage {
                                    source: "resources/notebook.svg"
                                    Layout.preferredHeight: compactLayout ? 44 : 70
                                    Layout.preferredWidth: compactLayout ? 44 : 70
                                    accentColor: theme.accent
                                }
                            }
                        }

                        GradientLine {
                            id: gradientLineE2E
                            colorSide: "transparent"
                            colorOutSide: theme.lowAccent
                            colorCenter: theme.accent
                            Layout.fillWidth: true
                            Layout.bottomMargin: compactLayout ? 4 : 12
                        }

                        Text {
                            id: e2eInfoText
                            text: qsTr("End-to-end encryption (E2EE) ensures that only you and your conversation partner can read your messages. No third parties, including servers, can access the content of your conversations. This is guaranteed by the use of modern encryption standards such as PQDH (Kyber1024 & X25519), SHA256, HKDF-SHA256, HMAC-SHA256, Ed25519, and Double Ratchet. Imagine that the server is just a relay; it sees absolutely no data except your public data signature key and metadata such as your IP address, interaction activity, and interaction direction. Use Tor or i2p to hide your IP address from the server. Use a password to encrypt all data on your side. \n\nWe cannot guarantee the confidentiality of correspondence if the device on which the client application data is stored is hacked.")
                            color: theme.accent
                            font.pixelSize: compactLayout ? 13 : 14
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            Layout.bottomMargin: compactLayout ? 16 : 24
                        }
                    }
                }
                Connections {
                    target: Platform.isAndroid && AndroidSystemUi.available ? AndroidSystemUi : null
                    function onKeyboardHeightChanged() {
                        if (AndroidSystemUi.keyboardHeight <= 0) {
                            mainWindow.lastKeyboardTarget = null
                            return
                        }
                        // activeFocusItem may be null if our IC-init fallback called
                        // requestFocus(qtEditText), temporarily clearing QML focus.
                        var focusItem = mainWindow.activeFocusItem || mainWindow.lastKeyboardTarget
                        if (!focusItem)
                            return
                        var pos = focusItem.mapToItem(columnLayout, 0, 0)
                        if (pos.y < 0 || pos.y >= columnLayout.height)
                            return
                        var itemBottom = pos.y + focusItem.height
                        var visibleBottom = scrollArea1.contentItem.contentY + scrollArea1.height
                        if (itemBottom > visibleBottom)
                            scrollArea1.contentItem.contentY = Math.max(0, itemBottom - scrollArea1.height + 20)
                    }
                }
            }
        }
    }
    Component { // warnPage - warning page explaining that the mnemonic phrase is the user's responsibility and must not be lost or shared
        id: warnPage
        Page {
            objectName: "warnPage"
            Rectangle {
                anchors.fill: parent
                color: theme.background
                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }
                Text {
                    id: warnText
                    text: qsTr("<b>Your mnemonic phrase is your responsibility.</b><br>Do not lose it and do not share it with anyone.")
                    anchors.top: parent.top
                    anchors.topMargin: 80
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: theme.accent
                    font.pixelSize: 16
                    font.family: "Roboto"
                    wrapMode: Text.WordWrap
                    textFormat: Text.RichText
                    width: parent.width * 0.8
                }
                GradientLine {
                    id: gradientLineWarn
                    anchors.top: warnText.bottom
                    anchors.topMargin: 20
                    anchors.horizontalCenter: parent.horizontalCenter
                    colorSide: "transparent"
                    colorOutSide: theme.halfAccent
                    colorCenter: theme.accent
                    width: warnText.width
                }
                ActionButton {
                    text: qsTr("I understand")
                    buttonColor: theme.accent
                    textColor: theme.background
                    anchors.top: gradientLineWarn.bottom
                    anchors.topMargin: 20
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.family: geologicaFont.name
                    font.weight: Font.Light
                    onClicked: {
                        stack.push(createPage, {
                            mnemonicphrase: CoreCrypto.generate_mnemonicphrase()
                        })
                    }
                }
            }
        }
    }
    Component { // createPage - page for creating a new mnemonic phrase
        id: createPage
        Page {
            objectName: "createPage"
            property string mnemonicphrase: qsTr("error generating mnemonic phrase")
            Rectangle {
                anchors.fill: parent
                color: theme.background
                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }
                Text {
                    id: warnText
                    text: qsTr("Your mnemonic phrase:<br><b>Write it down on paper or memorize it.</b>")
                    anchors.top: parent.top
                    anchors.topMargin: 80
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: theme.accent
                    font.pixelSize: 16
                    font.family: "Roboto"
                    wrapMode: Text.WordWrap
                    textFormat: Text.RichText
                    width: parent.width * 0.8
                }
                GradientLine {
                    id: gradientLineWarn
                    anchors.top: warnText.bottom
                    anchors.topMargin: 20
                    anchors.horizontalCenter: parent.horizontalCenter
                    colorSide: "transparent"
                    colorOutSide: theme.halfAccent
                    colorCenter: theme.accent
                    width: warnText.width
                }
                Text {
                    id: mnemonicText
                    text: mnemonicphrase
                    anchors.top: gradientLineWarn.bottom
                    anchors.topMargin: 20
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: theme.halfAccent
                    font.pixelSize: 14
                    font.family: "Roboto"
                    wrapMode: Text.WordWrap
                    width: parent.width * 0.8
                }
                ActionButton {
                    text: qsTr("I have saved my mnemonic phrase")
                    buttonColor: theme.accent
                    textColor: theme.background
                    anchors.top: mnemonicText.bottom
                    anchors.topMargin: 20
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.family: geologicaFont.name
                    font.weight: Font.Light
                    onClicked: {
                        Account.create(mnemonicphrase)
                        connections.addServers() // !!!
                        stack.push(profileSetupPage)
                    }
                }
            }
        }
    }
    Component { // profileSetupPage - first-time profile setup after creating a new mnemonic
        id: profileSetupPage
        Page {
            objectName: "profileSetupPage"
            Rectangle {
                anchors.fill: parent
                color: theme.background

                ScrollView {
                    id: profileScroll
                    WheelScroller { flickable: profileScroll.contentItem }
                    anchors.fill: parent
                    contentWidth: parent.width
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AlwaysOff }

                    ColumnLayout {
                        width: profileScroll.width * (Platform.isMobile ? 0.88 : 0.45)
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Platform.isMobile ? 50 : 24
                            color: "transparent"
                        }

                        Text {
                            text: qsTr("set up your profile")
                            font.pixelSize: Platform.isMobile ? 24 : 20
                            font.family: geologicaFont.name
                            font.weight: Font.ExtraBold
                            color: theme.accent
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Text {
                            text: qsTr("You can change this anytime in Settings")
                            font.pixelSize: 13
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            color: theme.halfAccent
                            Layout.alignment: Qt.AlignHCenter
                            topPadding: Platform.isMobile ? 6 : 4
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Platform.isMobile ? 32 : 14
                            color: "transparent"
                        }

                        Item {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: Platform.isMobile ? 90 : 70
                            Layout.preferredHeight: Platform.isMobile ? 90 : 70

                            Rectangle {
                                id: profileSetupAvatarCircle
                                anchors.fill: parent
                                radius: width / 2
                                color: theme.minimumAccent
                                layer.enabled: true

                                Text {
                                    id: profileSetupAvatarLetter
                                    anchors.centerIn: parent
                                    text: setupFirstNameField.inputText.length > 0
                                          ? setupFirstNameField.inputText[0].toUpperCase()
                                          : "?"
                                    font.pixelSize: Platform.isMobile ? 40 : 30
                                    font.family: geologicaFont.name
                                    color: theme.accent
                                    visible: !Account.hasAvatar("0")
                                }

                                Image {
                                    id: profileSetupAvatarImg
                                    anchors.fill: parent
                                    visible: false
                                    source: "image://avatars/0"
                                    mipmap: true
                                    fillMode: Image.PreserveAspectCrop
                                    smooth: true
                                    antialiasing: true
                                    layer.enabled: true
                                    layer.smooth: true
                                }

                                Rectangle {
                                    id: profileSetupAvatarMask
                                    anchors.fill: parent
                                    radius: parent.radius
                                    visible: false
                                    layer.enabled: true
                                    layer.smooth: true
                                }

                                MultiEffect {
                                    id: profileSetupAvatarEffect
                                    anchors.fill: parent
                                    source: profileSetupAvatarImg
                                    maskEnabled: true
                                    maskSource: profileSetupAvatarMask
                                    visible: Account.hasAvatar("0")
                                    smooth: true
                                    antialiasing: true
                                    maskSpreadAtMin: 1.0
                                    maskThresholdMin: 0.6
                                }

                                Connections {
                                    target: Account
                                    function onAvatarChanged() {
                                        profileSetupAvatarImg.source = "image://avatars/0?t=" + Date.now()
                                        profileSetupAvatarEffect.visible = Account.hasAvatar("0")
                                        profileSetupAvatarLetter.visible = !Account.hasAvatar("0")
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Account.selectAndSaveAvatar()
                                }
                            }

                            Rectangle {
                                width: Platform.isMobile ? 28 : 22
                                height: Platform.isMobile ? 28 : 22
                                radius: width / 2
                                color: theme.accent
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom

                                ColorImage {
                                    anchors.centerIn: parent
                                    width: Platform.isMobile ? 14 : 11
                                    height: Platform.isMobile ? 14 : 11
                                    source: "resources/photo.svg"
                                    accentColor: theme.background
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Account.selectAndSaveAvatar()
                                }
                            }
                        }

                        Text {
                            text: qsTr("tap to set avatar")
                            font.pixelSize: 11
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            color: theme.halfAccent
                            Layout.alignment: Qt.AlignHCenter
                            topPadding: Platform.isMobile ? 8 : 4
                        }

                        GradientLine {
                            Layout.topMargin: Platform.isMobile ? 28 : 14
                            Layout.fillWidth: true
                            colorSide: "transparent"
                            colorOutSide: theme.halfAccent
                            colorCenter: theme.accent
                        }

                        TextSetting {
                            id: setupFirstNameField
                            Layout.fillWidth: true
                            Layout.topMargin: Platform.isMobile ? 18 : 10
                            accentColor: theme.accent
                            lowAccentColor: theme.lowAccent
                            lowestAccentColor: theme.lowestAccent
                            backgroundColor: theme.background
                            settingText: qsTr("First Name")
                            settingName: "firstName"
                        }

                        TextSetting {
                            id: setupLastNameField
                            Layout.fillWidth: true
                            accentColor: theme.accent
                            lowAccentColor: theme.lowAccent
                            lowestAccentColor: theme.lowestAccent
                            backgroundColor: theme.background
                            settingText: qsTr("Last Name")
                            settingName: "lastName"
                        }

                        ManyTextSetting {
                            id: setupAboutMeField
                            Layout.fillWidth: true
                            accentColor: theme.accent
                            lowAccentColor: theme.lowAccent
                            lowestAccentColor: theme.lowestAccent
                            backgroundColor: theme.background
                            settingText: qsTr("About Me")
                            settingName: "aboutMe"
                        }

                        SettingsGroup {
                            accentColor: theme.accent
                            textColor: theme.halfAccent
                            groupText: qsTr("Unique name style")
                            Layout.fillWidth: true
                        }

                        NameStyleField {
                            id: setupNameStyleField
                            Layout.fillWidth: true
                            Layout.topMargin: 8
                            accentColor: theme.accent
                            lowAccentColor: theme.lowAccent
                            lowestAccentColor: theme.lowestAccent
                            backgroundColor: theme.background
                            previewFirstName: setupFirstNameField.inputText
                            previewLastName: setupLastNameField.inputText
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Platform.isMobile ? 32 : 14
                            color: "transparent"
                        }

                        ActionButton {
                            text: qsTr("get started")
                            buttonColor: theme.accent
                            textColor: theme.background
                            Layout.fillWidth: true
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            onClicked: {
                                Qt.inputMethod.commit()
                                let firstName = setupFirstNameField.inputText.trim()
                                const lastName = setupLastNameField.inputText.trim()
                                if (firstName.length === 0 && lastName.length === 0)
                                    firstName = qsTr("Anonymous")
                                Settings.setTextSetting("firstName", firstName)
                                Settings.setTextSetting("lastName", lastName)
                                Settings.setTextSetting("aboutMe", setupAboutMeField.inputText)
                                Settings.setTextSetting("nameStyle", setupNameStyleField.styleCss)
                                connections.translateAccountInfo()
                                stack.replace(null, mainPage)
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Platform.isMobile ? 32 : 14
                            color: "transparent"
                        }
                    }
                }
            }
        }
    }
    Component { // welcomePage - start page with greeting and language selection
        id: welcomePage
        Page {
            objectName: "welcomePage"
            Rectangle {
                anchors.fill: parent
                color: theme.background
                Logo {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: welcomeTitle.top
                    anchors.bottomMargin: 20
                    width: 90
                    height: 90
                    accentColor: theme.accent
                }
                Text {
                    id: welcomeTitle
                    text: qsTr("Patronus")
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: welcomeText.top
                    anchors.bottomMargin: 30
                    color: theme.accent
                    font.pixelSize: 40
                    font.family: geologicaFont.name
                    font.weight: Font.Medium
                }
                Text {
                    id: welcomeText
                    text: qsTr("reclaim your right to privacy in your correspondence")
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: gradientLine1.top
                    anchors.bottomMargin: 10
                    color: theme.accent
                    font.pixelSize: 26
                    font.family: geologicaFont.name
                    font.weight: Font.Light
                    width: parent.width * 0.8
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }
                GradientLine {
                    id: gradientLine1
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: 60
                    colorSide: "transparent"
                    colorOutSide: theme.halfAccent
                    colorCenter: theme.accent
                    width: welcomeText.width
                }
                LanguagesBox {
                    id: languageBox1

                    height: 50
                    width: 140

                    anchors.leftMargin: 20
                    anchors.topMargin: 20

                    transparentBool: true

                    backgroundColor: theme.background
                    accentColor: theme.accent
                    lowestAccentColor: mainWindow.adjustContrast(theme.accent, 1.7)
                    surfaceColor: mainWindow.chatPanelSurface
                    borderColor: mainWindow.chatPanelBorderColor
                }

                ActionButton {
                    text: qsTr("let`s go")
                    buttonColor: theme.accent
                    textColor: theme.background
                    anchors.top: gradientLine1.bottom
                    anchors.topMargin: 10
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.family: geologicaFont.name
                    font.weight: Font.Light
                    font.pixelSize: 14
                    height: 50
                    width: 100
                    onClicked: {
                        stack.push(regPage)
                    }
                }
            }
        }
    }
    Component { // addContactPage - page for adding a new contact by entering their public key and the server address where they are registered
        id: addContactPage
        Page {
            objectName: "addContactPage"
            property string pendingAddContactServer: ""
            property string pendingAddContactPubKey: ""
            property string pendingAddContactInvite: ""
            property bool addContactInProgress: false

            // Fills the advanced fields from a pasted/scanned contact code.
            // Returns true when the code is a valid Patronus contact string.
            function applyContactCode(text) {
                const parsed = Account.parseContactString(text)
                if (!parsed.valid) {
                    return false
                }
                advPubKeyField.text = parsed.pubKey
                advServerField.text = parsed.server
                advInviteField.text = parsed.invite
                return true
            }

            function finishAddContactWithError(message) {
                addContactInProgress = false
                pendingAddContactServer = ""
                pendingAddContactPubKey = ""
                pendingAddContactInvite = ""
                addContactStatusText.visible = true
                addContactStatusText.text = message
                addContactStatusText.color = theme.redAccent
                addContactLoadingImage.visible = false
            }

            function requestPendingContactPreKey() {
                if (!addContactInProgress || pendingAddContactServer === "" || pendingAddContactPubKey === "") {
                    return
                }

                if (!connections.isConnected(pendingAddContactServer) || !connections.isAuthenticated(pendingAddContactServer)) {
                    return
                }

                const serverId = pendingAddContactServer
                const contactPubKey = pendingAddContactPubKey
                const inviteHex = pendingAddContactInvite

                pendingAddContactServer = ""
                pendingAddContactPubKey = ""
                pendingAddContactInvite = ""

                addContactStatusText.text = qsTr("establishing end2end session...")

                connections.requestPreKey(serverId, contactPubKey, function(id, dh_pub, pq_pub, ed_pub, signature) {
                    if (id === false) {
                        finishAddContactWithError(qsTr("failed to request prekey from server"))
                        return
                    }
                    if (id === 404) {
                        finishAddContactWithError(qsTr("contact not found on server"))
                        return
                    }
                    if (id === 410) {
                        finishAddContactWithError(qsTr("this key has been revoked as compromised — do not trust it"))
                        return
                    }
                    if (id === 429) {
                        finishAddContactWithError(qsTr("server is rate-limiting prekey requests, try again shortly"))
                        return
                    }

                    console.log("Prekey id:", id)
                    addContactStatusText.text = qsTr("prekey received, starting session...")
                    addContactStatusText.color = theme.accent
                    addContactLoadingImage.visible = true

                    connections.startSessionWithPreKey(serverId, id, dh_pub, pq_pub, ed_pub, signature, contactPubKey, inviteHex, function(success) {
                        console.log("start session result:", success)
                        addContactLoadingImage.visible = false
                        addContactInProgress = false

                        if (success) {
                            // If we are still on the default placeholder home server,
                            // adopt the server we just joined as our home — we onboarded
                            // through this invite, so this is where we now live and
                            // receive. A user who already picked a real home keeps it.
                            const currentHome = mainWindow.sanitizeServerAddress(Settings.getTextSetting("serverAddress", ""))
                            if (currentHome === "" || currentHome === "localhost:443") {
                                Settings.setTextSetting("serverAddress", serverId)
                                connections.addServer(serverId)
                            }
                            addContactStatusText.text = qsTr("session started successfully")
                            addContactStatusText.color = theme.accent
                            connections.sendOwnAccountInfo(serverId, contactPubKey)
                            stack.pop()
                        } else {
                            addContactStatusText.text = qsTr("failed to start session, try again")
                            addContactStatusText.color = theme.redAccent
                        }
                    })
                })
            }

            Connections {
                target: connections

                function onAuthChanged(serverId, success) {
                    if (!addContactInProgress || serverId !== pendingAddContactServer || success !== true) {
                        return
                    }

                    requestPendingContactPreKey()
                }
            }

            Rectangle {
                anchors.fill: parent
                color: theme.background
                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }
                Rectangle {
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width * 0.9
                    anchors.topMargin: 50
                    anchors.leftMargin: 20
                    Text {
                        id: addContactText
                        text: qsTr("Start session with new contact")
                        anchors.top: parent.top
                        anchors.topMargin: 50
                        anchors.left: parent.left
                        color: theme.accent
                        font.pixelSize: 16
                        font.family: "Roboto"
                        wrapMode: Text.WordWrap
                    }
                    GradientLine {
                        id: gradientLineAddContact
                        anchors.top: addContactText.bottom
                        anchors.topMargin: 10
                        anchors.bottomMargin: 40
                        anchors.left: parent.left
                        colorSide: "transparent"
                        colorOutSide: theme.halfAccent
                        colorCenter: theme.accent
                        width: addContactText.width
                    }
                    Text {
                        id: addContactInfoText
                        text: qsTr("Scan your contact's QR code, or paste their contact code to add them.")
                        anchors.top: gradientLineAddContact.bottom
                        anchors.topMargin: 40
                        anchors.bottomMargin: 20
                        anchors.left: parent.left
                        color: theme.halfAccent
                        font.pixelSize: 14
                        font.family: "Roboto"
                        wrapMode: Text.WordWrap
                        width: parent.width * 0.9
                    }
                    Row {
                        id: qrActionsRow
                        anchors.top: addContactInfoText.bottom
                        anchors.topMargin: 18
                        anchors.left: parent.left
                        spacing: 10
                        ActionButton {
                            id: scanQrButton
                            text: qsTr("Scan QR")
                            buttonColor: theme.accent
                            textColor: theme.background
                            icon.source: "resources/qr.svg"
                            icon.color: theme.background
                            icon.width: 18
                            icon.height: 18
                            height: 44
                            font.pixelSize: 14
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            onClicked: {
                                const page = stack.push(scanQrPage)
                                if (page) {
                                    page.contactScanned.connect(function(payload) {
                                        inputContactCode.text = payload
                                        addContactStatusText.visible = true
                                        const parsed = Account.parseContactString(payload)
                                        if (parsed.valid && parsed.pubKey.toLowerCase() === Account.getPublicKey().toLowerCase()) {
                                            addContactStatusText.color = theme.redAccent
                                            addContactStatusText.text = qsTr("that's your own QR code — you can't add yourself")
                                        } else {
                                            addContactStatusText.color = theme.accent
                                            addContactStatusText.text = qsTr("QR scanned, press start session")
                                        }
                                    })
                                }
                            }
                        }
                        ActionButton {
                            id: myQrButton
                            text: qsTr("My QR")
                            buttonColor: theme.lowAccent
                            textColor: theme.accent
                            icon.source: "resources/qr.svg"
                            icon.color: theme.accent
                            icon.width: 18
                            icon.height: 18
                            height: 44
                            // Explicit floor rather than padding: the Material
                            // style's own implicit sizing was clipping the
                            // Cyrillic label ("Мой QR") regardless of
                            // leftPadding/rightPadding overrides.
                            width: Math.max(implicitWidth, 150)
                            font.pixelSize: 14
                            font.family: geologicaFont.name
                            font.weight: Font.Light
                            onClicked: stack.push(qrCodePage)
                        }
                    }
                    Text {
                        id: contactCodeLabel
                        text: qsTr("Contact code")
                        anchors.top: qrActionsRow.bottom
                        anchors.topMargin: 20
                        anchors.left: parent.left
                        color: theme.halfAccent
                        font.pixelSize: 12
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                    }
                    Rectangle {
                        id: inputContainer
                        width: parent.width * 0.9
                        height: 36
                        anchors.top: contactCodeLabel.bottom
                        anchors.topMargin: 6
                        anchors.left: parent.left
                        radius: Math.min(height / 2, 22)
                        color: Qt.tint(theme.background, Qt.rgba(1, 1, 1, 0.01))
                        PanelBorder {
                            edgeColor: mainWindow.chatPanelBorderColor
                        }
                        TextField {
                            id: inputContactCode
                            anchors.fill: parent
                            text: ""
                            leftPadding: 12
                            rightPadding: 12
                            selectedTextColor: theme.background
                            topPadding: 5
                            bottomPadding: 5
                            font.pixelSize: 14
                            font.family: "Roboto"
                            font.weight: Font.Light
                            horizontalAlignment: Text.AlignLeft
                            color: theme.accent
                            background: Rectangle {
                                color: "transparent"
                            }
                            Material.foreground: theme.accent
                            Material.background: theme.lowAccent
                            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhSensitiveData | Qt.ImhHiddenText
                            wrapMode: Text.Wrap
                            // Silently mirror a valid code into the advanced
                            // fields; validation errors surface on the start
                            // button, not on every keystroke.
                            onTextChanged: {
                                if (text.trim().length > 0)
                                    applyContactCode(text)
                            }
                            onActiveFocusChanged: {
                                if (activeFocus && Platform.isAndroid && AndroidSystemUi.available)
                                    Qt.callLater(function() {
                                        if (inputContactCode.activeFocus) {
                                            mainWindow.lastKeyboardTarget = inputContactCode
                                            AndroidSystemUi.showKeyboardWithHints(inputContactCode.inputMethodHints)
                                        }
                                    })
                            }
                        }
                    }
                    Text {
                        id: advancedToggle
                        text: advancedColumn.visible ? qsTr("hide details") : qsTr("show details")
                        anchors.top: inputContainer.bottom
                        anchors.topMargin: 12
                        anchors.left: parent.left
                        color: theme.halfAccent
                        font.pixelSize: 12
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        font.underline: true
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: advancedColumn.visible = !advancedColumn.visible
                        }
                    }
                    // Parsed pieces of the contact code (public key, invite,
                    // server), editable for power users who want to enter or
                    // tweak them manually.
                    Column {
                        id: advancedColumn
                        width: parent.width * 0.9
                        anchors.top: advancedToggle.bottom
                        anchors.topMargin: 10
                        anchors.left: parent.left
                        spacing: 6
                        visible: false
                        height: visible ? implicitHeight : 0

                        Text {
                            text: qsTr("Public key:")
                            color: theme.halfAccent
                            font.pixelSize: 12
                            font.family: "Roboto"
                        }
                        TextField {
                            id: advPubKeyField
                            width: parent.width
                            height: 30
                            leftPadding: 10
                            rightPadding: 10
                            topPadding: 5
                            bottomPadding: 5
                            font.pixelSize: 8
                            font.family: "Roboto"
                            font.weight: Font.Light
                            color: theme.accent
                            selectedTextColor: theme.background
                            background: Rectangle {
                                color: theme.minimumAccent
                                radius: 4
                            }
                            Material.foreground: theme.accent
                            Material.background: theme.lowAccent
                            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhSensitiveData | Qt.ImhHiddenText
                            onActiveFocusChanged: {
                                if (activeFocus && Platform.isAndroid && AndroidSystemUi.available)
                                    Qt.callLater(function() {
                                        if (advPubKeyField.activeFocus) {
                                            mainWindow.lastKeyboardTarget = advPubKeyField
                                            AndroidSystemUi.showKeyboardWithHints(advPubKeyField.inputMethodHints)
                                        }
                                    })
                            }
                        }
                        Text {
                            text: qsTr("Invite code:")
                            color: theme.halfAccent
                            font.pixelSize: 12
                            font.family: "Roboto"
                        }
                        TextField {
                            id: advInviteField
                            width: parent.width
                            height: 30
                            leftPadding: 10
                            rightPadding: 10
                            topPadding: 5
                            bottomPadding: 5
                            font.pixelSize: 8
                            font.family: "Roboto"
                            font.weight: Font.Light
                            color: theme.accent
                            selectedTextColor: theme.background
                            background: Rectangle {
                                color: theme.minimumAccent
                                radius: 4
                            }
                            Material.foreground: theme.accent
                            Material.background: theme.lowAccent
                            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhSensitiveData | Qt.ImhHiddenText
                            onActiveFocusChanged: {
                                if (activeFocus && Platform.isAndroid && AndroidSystemUi.available)
                                    Qt.callLater(function() {
                                        if (advInviteField.activeFocus) {
                                            mainWindow.lastKeyboardTarget = advInviteField
                                            AndroidSystemUi.showKeyboardWithHints(advInviteField.inputMethodHints)
                                        }
                                    })
                            }
                        }
                        Text {
                            text: qsTr("Server address:")
                            color: theme.halfAccent
                            font.pixelSize: 12
                            font.family: "Roboto"
                        }
                        TextField {
                            id: advServerField
                            width: parent.width
                            height: 30
                            text: {
                                const home = mainWindow.sanitizeServerAddress(Settings.getTextSetting("serverAddress", ""))
                                return home.length > 0 ? home : "localhost:443"
                            }
                            leftPadding: 10
                            rightPadding: 10
                            topPadding: 5
                            bottomPadding: 5
                            font.pixelSize: 14
                            font.family: "Roboto"
                            font.weight: Font.Light
                            color: theme.accent
                            selectedTextColor: theme.background
                            background: Rectangle {
                                color: theme.minimumAccent
                                radius: 4
                            }
                            Material.foreground: theme.accent
                            Material.background: theme.lowAccent
                            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhSensitiveData | Qt.ImhHiddenText
                            onActiveFocusChanged: {
                                if (activeFocus && Platform.isAndroid && AndroidSystemUi.available)
                                    Qt.callLater(function() {
                                        if (advServerField.activeFocus) {
                                            mainWindow.lastKeyboardTarget = advServerField
                                            AndroidSystemUi.showKeyboardWithHints(advServerField.inputMethodHints)
                                        }
                                    })
                            }
                        }
                    }
                    ActionButton {
                        id: startSessionButton
                        text: qsTr("start session")
                        buttonColor: theme.accent
                        textColor: theme.background
                        anchors.top: advancedColumn.bottom
                        anchors.topMargin: 20
                        anchors.left: parent.left
                        height: 44
                        font.pixelSize: 14
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        onClicked: { // !
                            // Flush any text still sitting in the IME composing region,
                            // then re-parse the pasted code (authoritative when present)
                            // and validate the resulting pieces exactly like the settings
                            // page does before we try to connect or store the contact.
                            Qt.inputMethod.commit()
                            if (inputContactCode.text.trim().length > 0
                                    && !applyContactCode(inputContactCode.text)
                                    && advPubKeyField.text.trim().length === 0) {
                                addContactStatusText.visible = true
                                addContactStatusText.text = qsTr("Invalid contact code")
                                addContactStatusText.color = theme.redAccent
                                return
                            }
                            const contactPubKey = advPubKeyField.text.trim().toLowerCase()
                            const inviteHex = advInviteField.text.trim().toLowerCase()
                            const cleanServer = mainWindow.sanitizeServerAddress(advServerField.text)
                            if (!/^[0-9a-f]{64}$/.test(contactPubKey)) {
                                addContactStatusText.visible = true
                                addContactStatusText.text = qsTr("Invalid contact code: bad public key")
                                addContactStatusText.color = theme.redAccent
                                return
                            }
                            // Adding ourselves would create a self-contact that can
                            // never establish a session (there is no second party to
                            // answer the session_request). Reject scanning/pasting our
                            // own contact code up front.
                            if (contactPubKey === Account.getPublicKey().toLowerCase()) {
                                addContactStatusText.visible = true
                                addContactStatusText.text = qsTr("you can't add yourself as a contact")
                                addContactStatusText.color = theme.redAccent
                                return
                            }
                            if (!/^[0-9a-f]{32}$/.test(inviteHex)) {
                                addContactStatusText.visible = true
                                addContactStatusText.text = qsTr("Invalid contact code: bad invite code")
                                addContactStatusText.color = theme.redAccent
                                return
                            }
                            if (!mainWindow.isValidServerAddress(cleanServer)) {
                                addContactStatusText.visible = true
                                addContactStatusText.text = qsTr("Invalid server address")
                                addContactStatusText.color = theme.redAccent
                                return
                            }
                            if (contactsModel.hasContact(cleanServer, contactPubKey)) {
                                addContactStatusText.visible = true
                                addContactStatusText.text = qsTr("contact already exists")
                                addContactStatusText.color = theme.redAccent
                                return
                            }
                            addContactInProgress = true
                            pendingAddContactServer = cleanServer
                            pendingAddContactPubKey = contactPubKey
                            pendingAddContactInvite = inviteHex
                            addContactLoadingImage.visible = true
                            addContactStatusText.visible = true
                            addContactStatusText.color = theme.accent
                            addContactStatusText.text = qsTr("connecting to server...")
                            connections.addServer(cleanServer)

                            if (connections.isConnected(cleanServer) && connections.isAuthenticated(cleanServer)) {
                                requestPendingContactPreKey()
                            }
                        }
                    }
                    ColorImage {
                        id: addContactLoadingImage
                        source: "resources/loading.svg"
                        width: 30
                        height: 30
                        anchors.left: startSessionButton.right
                        anchors.leftMargin: 20
                        accentColor: theme.accent
                        anchors.verticalCenter: startSessionButton.verticalCenter
                        visible: false
                        RotationAnimator on rotation {
                            from: 0
                            to: 360
                            duration: 800
                            running: true
                            loops: Animation.Infinite
                        }
                    }
                    Text {
                        id: addContactStatusText
                        text: qsTr("please wait...")
                        anchors.left: addContactLoadingImage.right
                        anchors.leftMargin: 15
                        anchors.verticalCenter: addContactLoadingImage.verticalCenter
                        color: theme.accent
                        font.pixelSize: Platform.isMobile ? 9 : 12
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        visible: false
                        width: parent.width - addContactStatusText.x - 40
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
    Component { // qrCodePage - shows the user's own contact QR (public key + home server) for other people to scan
        id: qrCodePage
        Page {
            id: qrRoot
            objectName: "qrCodePage"

            readonly property string homeServer: mainWindow.sanitizeServerAddress(Settings.getTextSetting("serverAddress", ""))
            property bool homeServerConnected: false

            function refreshStatus() {
                homeServerConnected = homeServer.length > 0
                    && connections.isConnected(homeServer)
                    && connections.isAuthenticated(homeServer)
            }

            // Shareable contact code: base64url(pubkey || invite || server ||
            // checksum), built in C++ (see inviteutils.h). Holding this code
            // grants the right to establish a session with us, so it should be
            // shared like a secret, not published.
            property string qrPayload: Account.contactShareString()

            Connections {
                target: Account
                function onInviteChanged() {
                    qrRoot.qrPayload = Account.contactShareString()
                }
            }

            readonly property color qrModuleColor: theme.accent

            // Sized off the smaller of the two dimensions so it can never
            // overflow the page regardless of window shape/orientation.
            readonly property real qrSide: Math.min(width, height) * (Platform.isMobile ? 0.7 : 0.32)

            // The QR itself is deliberately kept modest (qrSide above), but
            // tying the text/payload box to that same narrow width made them
            // look cramped, especially on wide desktop windows. Give them
            // their own, wider budget.
            readonly property real qrTextWidth: Platform.isMobile ? qrSide : Math.min(width * 0.6, 480)

            Component.onCompleted: refreshStatus()

            Connections {
                target: connections
                function onConnectionStatusChanged(serverId, status) {
                    if (serverId === qrRoot.homeServer)
                        qrRoot.refreshStatus()
                }
                function onAuthChanged(serverId, success) {
                    if (serverId === qrRoot.homeServer)
                        qrRoot.refreshStatus()
                }
            }

            Rectangle {
                anchors.fill: parent
                color: theme.background

                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }

                // No connection to the home server -> treat as "no home server".
                ColumnLayout {
                    visible: !qrRoot.homeServerConnected
                    anchors.centerIn: parent
                    width: parent.width * 0.8
                    spacing: 14
                    ColorImage {
                        Layout.alignment: Qt.AlignHCenter
                        source: "resources/qr.svg"
                        width: 48
                        height: 48
                        accentColor: theme.halfAccent
                        opacity: 0.5
                    }
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("QR generation is unavailable without an established home server.")
                        color: theme.halfAccent
                        font.pixelSize: 15
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        wrapMode: Text.WordWrap
                    }
                }

                // Home server is connected -> render the QR, centered on the page.
                ColumnLayout {
                    id: qrContentColumn
                    visible: qrRoot.homeServerConnected
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.verticalCenter: parent.verticalCenter
                    // On mobile a floating bottom nav pill (58 tall + 18 margin +
                    // safe-area inset) overlays the page bottom. Lift the centered
                    // column by half that footprint so its lowest element (the
                    // rotate/share status line) clears the pill instead of being
                    // pushed off the bottom of the screen.
                    anchors.verticalCenterOffset: Platform.isMobile
                        ? -((58 + 18 + mainWindow.safeBottomInset) / 2)
                        : 0
                    width: parent.width
                    spacing: 12

                    QrImageItem {
                        id: qrCodeCard
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: qrRoot.qrSide
                        Layout.preferredHeight: qrRoot.qrSide
                        text: qrRoot.qrPayload
                        foreground: qrRoot.qrModuleColor
                        background: "transparent"
                    }

                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: qrRoot.qrTextWidth
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("Let your contact scan this code to add you.")
                        color: theme.halfAccent
                        font.pixelSize: 13
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        wrapMode: Text.WordWrap
                    }

                    // The exact payload encoded in the QR, so the user can see and
                    // verify what they are handing out. Selectable for copying on
                    // desktop; tap-to-copy on mobile (for sending to camera-less
                    // desktop users).
                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: qrRoot.qrTextWidth
                        Layout.preferredHeight: qrPayloadText.implicitHeight + 16
                        radius: Math.min(height / 2, 22)
                        color: Qt.tint(theme.background, Qt.rgba(1, 1, 1, 0.01))
                        PanelBorder {
                            edgeColor: mainWindow.chatPanelBorderColor
                        }
                        TextEdit {
                            id: qrPayloadText
                            anchors.fill: parent
                            anchors.margins: 8
                            text: qrRoot.qrPayload
                            readOnly: true
                            // Click-to-copy (below) covers the whole field on every
                            // platform, so mouse selection is disabled to avoid the
                            // two fighting over the click.
                            selectByMouse: false
                            wrapMode: TextEdit.WrapAnywhere
                            horizontalAlignment: Text.AlignHCenter
                            color: theme.accent
                            font.pixelSize: 11
                            font.family: "Roboto"
                            selectionColor: theme.accent
                            selectedTextColor: theme.background
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                ClipboardHelper.setText(qrRoot.qrPayload)
                                shareStatus.show(qsTr("Contact code copied"))
                            }
                        }
                    }

                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: Platform.isMobile ? qsTr("tap the code to copy it")
                                                : qsTr("click the code to copy it")
                        color: theme.halfAccent
                        font.pixelSize: 11
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                    }

                    ActionButton {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 6
                        text: qsTr("Share QR")
                        buttonColor: theme.accent
                        textColor: theme.background
                        icon.source: "resources/share.svg"
                        icon.color: theme.background
                        icon.width: 16
                        icon.height: 16
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        onClicked: qrRoot.shareQr()
                    }

                    // Rotating the invite invalidates every previously shared
                    // QR/contact code for NEW sessions (the escape hatch when a
                    // code leaks); already-established chats are unaffected.
                    ActionButton {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("Rotate invite")
                        buttonColor: theme.lowAccent
                        textColor: theme.accent
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        onClicked: {
                            Account.rotateInviteSecret()
                            shareStatus.show(qsTr("Invite rotated — previously shared codes no longer work"))
                        }
                    }

                    // Reserves a fixed 3-line-tall slot regardless of whether
                    // a status message is showing, so it fading in/out never
                    // shifts qrContentColumn's centered layout. Width-capped
                    // and wrapped since longer translations (e.g. Russian)
                    // would otherwise overflow the page edges.
                    FontMetrics {
                        id: shareStatusMetrics
                        font: shareStatus.font
                    }
                    Text {
                        id: shareStatus
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: qrRoot.qrTextWidth
                        Layout.preferredHeight: shareStatusMetrics.lineSpacing * 3
                        text: ""
                        opacity: text.length > 0 ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.WordWrap
                        color: theme.halfAccent
                        font.pixelSize: 11
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        function show(message) {
                            text = message
                            shareStatusTimer.restart()
                        }
                        Timer {
                            id: shareStatusTimer
                            interval: 2500
                            onTriggered: shareStatus.text = ""
                        }
                    }
                }
            }

            // Renders the QR card to a PNG and hands it to the platform share
            // sheet (Android) or, on desktop, copies it to the clipboard.
            function shareQr() {
                qrCodeCard.grabToImage(function(result) {
                    const path = AndroidSystemUi.shareableFilePath("patronus_qr.png")
                    if (!path || !result.saveToFile(path)) {
                        shareStatus.show(qsTr("Failed to prepare QR image"))
                        return
                    }
                    if (Platform.isAndroid && AndroidSystemUi.available
                            && AndroidSystemUi.shareImage(path, qrRoot.qrPayload)) {
                        return
                    }
                    if (ClipboardHelper.setImageFromFile(path)) {
                        shareStatus.show(qsTr("QR image copied to clipboard"))
                    } else {
                        shareStatus.show(qsTr("Failed to share QR"))
                    }
                })
            }
        }
    }
    Component { // scanQrPage - scans a contact's QR (public key + server) with the camera and fills the add-contact form
        id: scanQrPage
        Page {
            id: scanRoot
            objectName: "scanQrPage"

            signal contactScanned(string payload)

            property string statusMessage: qsTr("Point the camera at a QR code")
            readonly property bool hasCamera: mediaDevices.videoInputs.length > 0

            function pickCamera() {
                const inputs = mediaDevices.videoInputs
                if (inputs.length === 0)
                    return null
                for (let i = 0; i < inputs.length; i++) {
                    if (inputs[i].position === CameraDevice.BackFace)
                        return inputs[i]
                }
                return inputs[0]
            }

            function startCamera() {
                if (!hasCamera) {
                    statusMessage = qsTr("No camera available")
                    return
                }
                const dev = pickCamera()
                if (dev)
                    camera.cameraDevice = dev
                camera.active = true
                qrScanner.active = true
            }

            function stopCamera() {
                qrScanner.active = false
                camera.active = false
            }

            function handleScanned(payload) {
                const parsed = Account.parseContactString(payload)
                if (!parsed.valid
                        || !mainWindow.isValidServerAddress(mainWindow.sanitizeServerAddress(parsed.server))) {
                    statusMessage = qsTr("This QR code is not a Patronus contact")
                    qrScanner.active = true
                    return
                }
                stopCamera()
                scanRoot.contactScanned(payload)
                stack.pop()
            }

            Component.onCompleted: qrScanner.requestCameraPermission()
            Component.onDestruction: stopCamera()

            MediaDevices { id: mediaDevices }

            CaptureSession {
                id: captureSession
                camera: Camera {
                    id: camera
                    active: false
                }
                videoOutput: videoOutput
            }

            QrScanner {
                id: qrScanner
                videoSink: videoOutput.videoSink
                onCodeScanned: (text) => scanRoot.handleScanned(text)
                onCameraPermissionGranted: scanRoot.startCamera()
                onCameraPermissionDenied:
                    scanRoot.statusMessage = qsTr("Camera permission is required to scan QR codes")
            }

            Rectangle {
                anchors.fill: parent
                color: theme.background

                VideoOutput {
                    id: videoOutput
                    anchors.centerIn: parent
                    width: Platform.isMobile ? parent.width : Math.min(parent.width * 0.8, parent.height * 0.7)
                    height: Platform.isMobile ? parent.height : width
                    fillMode: VideoOutput.PreserveAspectCrop
                    visible: camera.active
                }

                // Themed viewfinder frame over the preview.
                Rectangle {
                    anchors.centerIn: videoOutput
                    width: Math.min(videoOutput.width, videoOutput.height) * 0.7
                    height: width
                    color: "transparent"
                    border.color: theme.accent
                    border.width: 3
                    radius: 12
                    visible: camera.active
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Platform.isMobile ? 70 : 45
                    width: parent.width * 0.85
                    horizontalAlignment: Text.AlignHCenter
                    text: scanRoot.statusMessage
                    color: theme.accent
                    font.pixelSize: 14
                    font.family: geologicaFont.name
                    font.weight: Font.Light
                    wrapMode: Text.WordWrap
                    style: Text.Outline
                    styleColor: theme.background
                }

                BackButton {
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }
            }
        }
    }
    Component { // regPage - registration page where the user enters a mnemonic phrase to access signing keys or creates a new one if needed
        id: regPage
        Page {
            objectName: "regPage"
            Rectangle {
                anchors.fill: parent
                color: theme.background
                Text { // warning that the mnemonic phrase is the user's responsibility and must not be lost or shared
                    id: regText
                    text: qsTr("Patronus uses a mnemonic phrase to access your signature keys.<br><b><u>No phone numbers or email addresses!</u></b><br><br>Your mnemonic phrase is your responsibility. Do not lose it and do not share it with anyone.")
                    anchors.top: parent.top
                    anchors.topMargin: 80
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: theme.halfAccent
                    font.pixelSize: 16
                    font.family: "Roboto"
                    wrapMode: Text.WordWrap
                    width: parent.width * 0.8
                    textFormat: Text.RichText
                }
                Rectangle { // chat-composer-styled container for the mnemonic field
                    id: mnemonicInputContainer
                    anchors.top: regText.bottom
                    anchors.topMargin: 20
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: regText.width
                    height: 50
                    radius: Math.min(height / 2, 22)
                    color: Qt.tint(theme.background, Qt.rgba(1, 1, 1, 0.01))

                    PanelBorder {
                        edgeColor: mainWindow.chatPanelBorderColor
                    }

                    TextField {
                        id: mnemonicInput
                        anchors.fill: parent
                        leftPadding: 14
                        rightPadding: 14
                        verticalAlignment: TextInput.AlignVCenter
                        selectedTextColor: theme.background
                        font.pixelSize: 14
                        font.family: "Roboto"
                        font.weight: Font.Light
                        color: theme.accent
                        placeholderText: qsTr("Enter mnemonic phrase here")
                        placeholderTextColor: theme.accent
                        inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase | Qt.ImhSensitiveData | Qt.ImhHiddenText
                        Material.foreground: theme.accent
                        Material.background: "transparent"
                        background: Rectangle { color: "transparent" }
                        onActiveFocusChanged: {
                            if (activeFocus && Platform.isAndroid && AndroidSystemUi.available)
                                Qt.callLater(function() {
                                    if (mnemonicInput.activeFocus) {
                                        mainWindow.lastKeyboardTarget = mnemonicInput
                                        AndroidSystemUi.showKeyboardWithHints(mnemonicInput.inputMethodHints)
                                    }
                                })
                        }
                    }
                }
                RowLayout { // buttons for creating a new mnemonic phrase or continuing with the entered one
                    anchors.top: mnemonicInputContainer.bottom
                    anchors.topMargin: 20
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 10
                    height: 40
                    ActionButton { // create new
                        text: qsTr("create new")
                        buttonColor: theme.accent
                        textColor: theme.background
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        onClicked: {
                            stack.push(warnPage)
                        }
                    }
                    ActionButton { // continue
                        text: qsTr("continue")
                        buttonColor: theme.accent
                        textColor: theme.background
                        enabled: mnemonicInput.text.length > 0
                        font.family: geologicaFont.name
                        font.weight: Font.Light
                        onClicked: { // !
                            if(Account.create(mnemonicInput.text)) {
                                connections.addServers()
                                stack.push(mainPage)
                            } else {
                                mnemonicInput.text = ""
                                mnemonicInput.placeholderText = qsTr("Invalid mnemonic phrase, try again")
                            }
                        }
                    }
                }
                BackButton { // back button
                    id: backButton
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.topMargin: 20
                    anchors.leftMargin: 20
                }
            }
        }
    }
    Rectangle { // bottom panel
        id: bottomPanel
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        z: 300
        color: theme.background
        visible: !Platform.isMobile
        height: Platform.isMobile ? 0 : 25
        width: parent.width
        RowLayout { 
            id: bottomPanel2
            height: 25
            width: parent.width
            spacing: 0
            visible: !Platform.isMobile && stack.currentItem && stack.currentItem.objectName !== "welcomePage" && stack.currentItem.objectName !== "messageStatusInfoPage" && stack.currentItem.objectName !== "sessionFingerprintPage" && stack.currentItem.objectName !== "regPage" && stack.currentItem.objectName !== "warnPage" && stack.currentItem.objectName !== "createPage" && stack.currentItem.objectName !== "profileSetupPage" && stack.currentItem.objectName !== "e2eInfoPage" && stack.currentItem.objectName !== "changePasswordPage" && stack.currentItem.objectName !== "passwordPage" && stack.currentItem.objectName !== "addContactPage" && stack.currentItem.objectName !== "scanQrPage" && stack.currentItem.objectName !== "donatePage"
            BottomButton { // settingsButton
                id: settingsButton
                Layout.alignment: Qt.AlignLeft
                icon.source: "resources/settings.svg"
                icon.width: 13
                icon.height: 13
                icon.color: theme.accent
                property int rotationAngle: 0
                onClicked: {
                    if (stack.currentItem && stack.currentItem.objectName === "settingsPage") {
                        rotateAnim.start()
                        stack.pop()
                        return
                    }
                    rotateAnim.start()
                    stack.push(settingsPage)
                }
                rotation: rotationAngle
                PropertyAnimation {
                    id: rotateAnim
                    target: settingsButton
                    property: "rotation"
                    from: 0
                    to: 180
                    duration: 200
                    running: false
                    onStopped: settingsButton.rotation = 0
                }
            }
            BottomButton { // donateButton
                id: donateButton
                Layout.alignment: Qt.AlignLeft
                icon.source: "resources/donate.svg"
                icon.width: 13
                icon.height: 13
                icon.color: theme.accent
                property int rotationAngle: 0
                onClicked: {
                    if (stack.currentItem && stack.currentItem.objectName === "donatePage") {
                        stack.pop()
                        rotateAnim.start()
                        return
                    }
                    stack.push(donatePage)
                }
            }
            BottomButton { // updatesButton
                id: updatesButton
                Layout.alignment: Qt.AlignLeft
                icon.source: "resources/update.svg"
                icon.width: 13
                icon.height: 13
                icon.color: theme.accent
                onClicked: {
                    if (stack.currentItem && stack.currentItem.objectName === "updatesPage") {
                        stack.pop()
                        return
                    }
                    stack.push(updatesPage)
                }
            }
            BottomButton { // myQrButton
                id: myQrBottomButton
                Layout.alignment: Qt.AlignLeft
                icon.source: "resources/qr.svg"
                icon.width: 13
                icon.height: 13
                icon.color: theme.accent
                onClicked: {
                    if (stack.currentItem && stack.currentItem.objectName === "qrCodePage") {
                        stack.pop()
                        return
                    }
                    stack.push(qrCodePage)
                }
            }
            BottomButton { // troubleshootButton
                id: troubleshootBottomButton
                Layout.alignment: Qt.AlignLeft
                icon.source: "resources/internet.svg"
                icon.width: 13
                icon.height: 13
                icon.color: theme.accent
                onClicked: {
                    if (stack.currentItem && stack.currentItem.objectName === "troubleshootingPage") {
                        stack.pop()
                        return
                    }
                    stack.push(troubleshootingPage)
                }
            }
            property string serverAddress: Settings.getTextSetting("serverAddress", qsTr("Not connected"))
            property bool serverAuthed: false
            property int serverPingMs: -1
            property bool prekeyGenerationActive: false
            property string prekeyGenerationText: ""

            Component.onCompleted: {
                bottomPanel2.serverAuthed = connections.isConnected(bottomPanel2.serverAddress) && connections.isAuthenticated(bottomPanel2.serverAddress)
                bottomPanel2.prekeyGenerationActive = connections.isPrekeyGenerationActive(bottomPanel2.serverAddress)
                bottomPanel2.prekeyGenerationText = connections.prekeyGenerationStatusText(bottomPanel2.serverAddress)
            }
            
            property int connectionStatusVersion: 0
            Connections { // !
                target: Settings
                function onSettingChanged(settingName, value) {
                    if (settingName === "serverAddress") {
                        bottomPanel2.serverAddress = value
                        bottomPanel2.serverAuthed = connections.isConnected(value) && connections.isAuthenticated(value)
                        bottomPanel2.serverPingMs = -1
                        bottomPanel2.prekeyGenerationActive = connections.isPrekeyGenerationActive(value)
                        bottomPanel2.prekeyGenerationText = connections.prekeyGenerationStatusText(value)
                        bottomPanel2.connectionStatusVersion += 1
                    }
                }
            }
            Connections { // !
                target: connections
                function onAuthChanged(serverId, success) {
                    if (serverId === bottomPanel2.serverAddress) {
                        bottomPanel2.serverAuthed = success
                        bottomPanel2.prekeyGenerationActive = connections.isPrekeyGenerationActive(serverId)
                        bottomPanel2.prekeyGenerationText = connections.prekeyGenerationStatusText(serverId)
                        bottomPanel2.connectionStatusVersion += 1
                    }
                }
                function onPrekeyGenerationStateChanged(serverId, active, generatedCount, targetCount) {
                    if (serverId === bottomPanel2.serverAddress) {
                        bottomPanel2.prekeyGenerationActive = active
                        bottomPanel2.prekeyGenerationText = active
                            ? qsTr("Generating prekeys")
                            : ""
                        bottomPanel2.connectionStatusVersion += 1
                    }
                }
                function onPongReceived(serverId, elapsedTime) {
                    if (serverId === bottomPanel2.serverAddress) {
                        bottomPanel2.serverPingMs = elapsedTime
                        bottomPanel2.connectionStatusVersion += 1
                    }
                }
            }
            RowLayout {
                id: connectionInfoRow
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Text {
                    id: prekeyGenerationStatusText
                    text: bottomPanel2.prekeyGenerationText
                    visible: bottomPanel2.prekeyGenerationActive && text.length > 0
                    color: theme.lowAccent
                    font.pixelSize: 10
                    font.family: geologicaFont.name
                    font.weight: Font.Light
                    horizontalAlignment: Text.AlignRight
                    Layout.alignment: Qt.AlignVCenter
                    Layout.rightMargin: 8
                    property int _forceUpdate: bottomPanel2.connectionStatusVersion
                }
                Text { // serverStatusText
                    id: serverStatusText
                    text: bottomPanel2.serverAddress
                    color: bottomPanel2.serverAuthed ? theme.accent : theme.lowAccent
                    font.pixelSize: 10
                    font.family: geologicaFont.name
                    font.weight: Font.Light
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.alignment: Qt.AlignVCenter
                    Layout.rightMargin: 5
                    property int _forceUpdate: bottomPanel2.connectionStatusVersion
                }
                Text { // serverStatus
                    id: serverStatus
                    text: qsTr("Connecting...")
                    color: theme.accent
                    font.pixelSize: 10
                    font.family: geologicaFont.name
                    font.weight: Font.Light
                    horizontalAlignment: Text.AlignLeft
                    Layout.alignment: Qt.AlignVCenter
                    Layout.rightMargin: 5
                }
                Text { // serverPingText
                    id: serverPingText
                    text: bottomPanel2.serverPingMs >= 0 ? (bottomPanel2.serverPingMs + " ms") : "0 ms"
                    visible: bottomPanel2.serverPingMs >= 0
                    color: (bottomPanel2.serverPingMs >= 0 && bottomPanel2.serverPingMs < 500) ? theme.accent : theme.redAccent
                    font.pixelSize: 10
                    font.family: geologicaFont.name
                    font.weight: Font.Light
                    horizontalAlignment: Text.AlignRight
                    Layout.alignment: Qt.AlignVCenter
                    Layout.leftMargin: 5
                    Layout.rightMargin: 5
                    property int _forceUpdate: bottomPanel2.connectionStatusVersion
                }
                Rectangle { // serverStatusIndicator
                    id: serverStatusIndicator
                    color: bottomPanel2.serverAuthed ? "transparent" : theme.lowAccent
                    gradient: bottomPanel2.serverAuthed ? statusIndicatorGradient : null
                    radius: height / 2
                    antialiasing: true
                    layer.enabled: true
                    layer.smooth: true
                    Layout.preferredWidth: 10
                    Layout.preferredHeight: 10
                    Layout.leftMargin: 0
                    Layout.rightMargin: 5
                    Layout.alignment: Qt.AlignVCenter

                    Gradient {
                        id: statusIndicatorGradient
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: "#00c4dc" }
                        GradientStop { position: 1.0; color: "#00ef96" }
                    }
                }
            }
            Connections { // ! issue: there can be multiple servers; revisit this
                target: MainSignals
                function onSyncStarted(serverId) {
                    if (serverId === bottomPanel2.serverAddress) {
                        console.log("Sync started signal received in QML.");
                        serverStatus.text = qsTr("Synchronizing...")
                    }
                }
                function onSyncEnded(serverId) {
                    if (serverId === bottomPanel2.serverAddress) {
                        console.log("Sync ended signal received in QML.");
                        serverStatus.text = qsTr("Connected")
                    }
                }
            }
        } 
    }
}
