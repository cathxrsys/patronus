pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Effects
import QtQuick.Layouts
import QtMultimedia

Rectangle {
    id: mainWindow

    anchors.fill: parent
    z: 10000
    focus: true
    color: mainWindow.theme.background

    FontLoader {
        id: geologicaFont
        source: "resources/fonts/geologica.ttf"
    }

    property string themeId: Settings.getTextSetting("themeKey", themeCatalog.defaultTheme)
    property var theme: themeLoader.item

    property string type: "incoming"
    property string status: "calling"
    property string firstName: "Anonymous"
    property string lastName: contactsModel.getLastNameByKey(contactServer, contactPubKey)
    property string contactNameStyle: contactsModel.getNameStyleByKey(contactServer, contactPubKey)
    property string avatarSource: "resources/avatars/anonymous.svg"
    property string descriptionCall: qsTr("If this number matches yours and your conversation partner's, then your call is secure.")
    property bool descriptionCallVisible: true
    property string mainTitleText: qsTr("Incoming Call")
    property string mainCallInfo: "0 0 0 0  0 0 0 0"
    property string contactPubKey: ""
    property string contactServer: ""
    property real voiceLevel: 0.0
    property string dh_pub: ""
    property string dh_id: ""
    property real callId: 0
    readonly property bool speakerButtonVisible: mainWindow.status === "active" && AndroidSystemUi.available
    readonly property bool compactLayout: width < 420 || height < 760
    readonly property real horizontalPadding: compactLayout ? 16 : 24
    readonly property real verticalPadding: compactLayout ? 16 : 24
    // Safe-area insets for the edge-to-edge call surface (embedded full-window in
    // the main window's contentItem, so it draws under the system bars on Android
    // 15+). Keep the header off the status bar and the action buttons off the
    // navigation bar. 0 on older Android, where the window is already inset.
    readonly property real safeTopInset: (AndroidSystemUi.available && AndroidSystemUi.edgeToEdge) ? mainWindow.SafeArea.margins.top : 0
    // Trim a little off the reserved nav-bar space (clamped at 0) so the action
    // buttons sit closer to the bar without going under it — matches Main.qml.
    readonly property real safeBottomInset: (AndroidSystemUi.available && AndroidSystemUi.edgeToEdge) ? Math.max(0, mainWindow.SafeArea.margins.bottom - 18) : 0
    readonly property real contentWidth: Math.min(width - horizontalPadding * 2, compactLayout ? 320 : 360)
    readonly property real heroSize: compactLayout ? 120 : 140
    readonly property real heroSectionHeight: compactLayout ? 170 : 210
    readonly property real buttonSize: compactLayout ? 88 : 100
    readonly property real buttonSpacing: compactLayout ? 48 : 80
    readonly property real headerHeight: 48
    readonly property real bottomActionsHeight: buttonSize + 20
    property var settingsManager: Settings
    property var callController: callManager
    property var connectionBridge: connections
    property var accountManager: Account
    // Set by CallManager when the call was surfaced from an FCM push: the system
    // notification ringer is already ringing, so we must not also ring in-app.
    property bool externalRing: false

    function isValidAvatarSource(source) {
        return !!source && source !== "||"
    }

    function resolvedAvatarSource() {
        const contactAvatar = mainWindow.accountManager
            ? mainWindow.accountManager.getContactAvatarSource(mainWindow.contactServer, mainWindow.contactPubKey)
            : ""

        if (mainWindow.isValidAvatarSource(contactAvatar))
            return contactAvatar + "?t=" + Date.now()
        if (mainWindow.isValidAvatarSource(mainWindow.avatarSource))
            return mainWindow.avatarSource
        return ""
    }

    function fallbackInitial() {
        return mainWindow.firstName && mainWindow.firstName.length > 0 ? mainWindow.firstName[0] : "?"
    }

    readonly property string pendingRingPath: {
        if (mainWindow.type === "outgoing" && mainWindow.status === "calling")
            return ":/qt/qml/client/resources/sounds/outgoing_calling.wav"
        if (mainWindow.type === "incoming" && mainWindow.status === "request")
            return ":/qt/qml/client/resources/sounds/incoming_call_Default.wav"
        return ""
    }

    onPendingRingPathChanged: {
        if (!AndroidSystemUi.available) return
        // For FCM-surfaced incoming calls the notification ringer already rings;
        // skip the in-app ringtone/vibration to avoid a double ring. Outgoing
        // calls are never external, so they always ring here.
        if (pendingRingPath !== "" && !(mainWindow.externalRing && mainWindow.type === "incoming")) {
            if (Settings.getBoolSetting("callSoundsEnabled", true))
                AndroidSystemUi.startRingtone(pendingRingPath, mainWindow.type === "outgoing")
            if (Settings.getBoolSetting("callVibrationEnabled", true) && mainWindow.type === "incoming")
                AndroidSystemUi.startCallVibration(0)
        } else {
            AndroidSystemUi.stopRingtone()
            AndroidSystemUi.stopCallVibration()
        }
    }

    function stopRingSounds() {
        callingSound.stop()
        requestSound.stop()
        if (AndroidSystemUi.available) {
            AndroidSystemUi.stopRingtone()
            AndroidSystemUi.stopCallVibration()
        }
    }

    function dismissCallView(playDiscardTone) {
        stopRingSounds()

        if (playDiscardTone)
            discardSound.play()

        if (mainWindow.callController && mainWindow.callController.dismissCallUi)
            mainWindow.callController.dismissCallUi()
        else
            mainWindow.visible = false
    }

    function closeCallView() {
        stopRingSounds()

        if (mainWindow.type === "error") {
            mainWindow.dismissCallView(true)
            return
        }

        discardSound.play()

        // The caller hanging up before the call is answered is a missed call
        // (call_cancel); ending an active call is a normal hang up
        // (call_discard).
        if (mainWindow.type === "outgoing" && mainWindow.status !== "active") {
            mainWindow.callController.cancel(
                mainWindow.contactServer,
                mainWindow.contactPubKey,
                mainWindow.callId,
                true,
                mainWindow.status
            )
        } else {
            mainWindow.callController.discard(
                mainWindow.contactServer,
                mainWindow.contactPubKey,
                mainWindow.callId,
                mainWindow.type === "outgoing",
                mainWindow.status
            )
        }
    }

    function acceptCallView() {
        stopRingSounds()
        acceptSound.play()

        mainWindow.status = "active"
        mainWindow.descriptionCallVisible = true

        mainWindow.callController.accept(
            mainWindow.contactServer,
            mainWindow.contactPubKey,
            mainWindow.dh_pub,
            mainWindow.dh_id,
            mainWindow.callId
        )

        mainWindow.mainCallInfo = mainWindow.connectionBridge.getCallKeysFingerprint()
    }

    // The view can be torn down directly from C++ (e.g. when the caller cancels
    // an incoming call). Make sure the ringtone and vibration always stop.
    Component.onDestruction: mainWindow.stopRingSounds()

    ThemeCatalog {
        id: themeCatalog
    }

    Loader {
        id: themeLoader
        asynchronous: false
        active: true
        source: themeCatalog.sourceFor(mainWindow.themeId)
    }

    onThemeIdChanged: {
        themeLoader.source = themeCatalog.sourceFor(mainWindow.themeId)
    }

    SoundEffect {
        id: callingSound
        source: "resources/sounds/outgoing_calling.wav"
        loops: SoundEffect.Infinite
        onStatusChanged: {
            if (AndroidSystemUi.available) return
            if (status === SoundEffect.Ready && mainWindow.type === "outgoing" && mainWindow.status === "calling"
                    && Settings.getBoolSetting("callSoundsEnabled", true))
                play()
        }
    }

    SoundEffect {
        id: requestSound
        source: "resources/sounds/incoming_call_Default.wav"
        loops: SoundEffect.Infinite
        onStatusChanged: {
            if (AndroidSystemUi.available) return
            if (status === SoundEffect.Ready && mainWindow.type === "incoming" && mainWindow.status === "request"
                    && Settings.getBoolSetting("callSoundsEnabled", true))
                play()
        }
    }

    SoundEffect {
        id: discardSound
        source: "resources/sounds/call_discard.wav"
        loops: 0
    }

    SoundEffect {
        id: acceptSound
        source: "resources/sounds/accept_call.wav"
        loops: 0
    }

    Connections {
        target: mainWindow.connectionBridge

        function onCallAcceptReceived() {
            stopRingSounds()
            acceptSound.play()
            mainWindow.status = "active"
            mainWindow.mainCallInfo = mainWindow.connectionBridge.getCallKeysFingerprint()
            mainWindow.descriptionCallVisible = true
        }

        function onCallDiscardReceived(serverId, contactPubKey, callId, reason) {
            if (mainWindow.contactServer === serverId
                    && mainWindow.contactPubKey === contactPubKey
                    && mainWindow.callId === callId) {
                mainWindow.stopRingSounds()
                if (mainWindow.type === "error") {
                    mainWindow.dismissCallView(true)
                    return
                }
                discardSound.play()
                // The callee was busy on another call. Show "Contact is busy" and
                // leave teardown to CallManager, which keeps the view up briefly;
                // do not discard() here, to avoid a double teardown.
                if (reason === "busy") {
                    mainWindow.mainCallInfo = qsTr("Contact is busy")
                    return
                }
                // discard() records the "Discarded Call" message and closes the
                // view. The contact who was called declined it.
                mainWindow.callController.discard(
                    mainWindow.contactServer,
                    mainWindow.contactPubKey,
                    mainWindow.callId,
                    mainWindow.type === "outgoing",
                    mainWindow.status
                )
            }
        }

        // The caller cancelled (missed call). CallManager records the missed
        // call in C++; here we just stop ringing and dismiss the view.
        function onCallCancelReceived(serverId, contactPubKey, callId) {
            if (mainWindow.contactServer === serverId
                    && mainWindow.contactPubKey === contactPubKey
                    && mainWindow.callId === callId) {
                mainWindow.dismissCallView(true)
            }
        }
    }

    Keys.onReleased: function(event) {
        if (event.key === Qt.Key_Back || event.key === Qt.Key_Escape) {
            event.accepted = true
            mainWindow.closeCallView()
        }
    }

    MouseArea {
        anchors.fill: parent
    }

    Rectangle {
        anchors.fill: parent
        color: mainWindow.theme.background
    }

    Item {
        anchors.fill: parent
        anchors.leftMargin: mainWindow.horizontalPadding
        anchors.rightMargin: mainWindow.horizontalPadding
        anchors.topMargin: mainWindow.horizontalPadding + mainWindow.safeTopInset
        anchors.bottomMargin: mainWindow.horizontalPadding + mainWindow.safeBottomInset

        Row {
            id: headerRow
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: mainWindow.headerHeight
            spacing: 0

            // Text {
            //     width: headerRow.width - closeButton.width
            //     height: parent.height
            //     verticalAlignment: Text.AlignVCenter
            //     text: qsTr("Call")
            //     color: mainWindow.theme.accent
            //     font.pixelSize: 18
            //     font.family: geologicaFont.name
            //     font.weight: Font.Bold
            // }

            // PanelButton {
            //     id: closeButton
            //     width: 48
            //     height: 48
            //     anchors.verticalCenter: parent.verticalCenter
            //     icon.source: "resources/close.svg"
            //     icon.color: mainWindow.theme.accent
            //     icon.width: 18
            //     icon.height: 18
            //     onClicked: mainWindow.closeCallView()
            // }
        }

        Column {
            id: contentColumn
            width: mainWindow.contentWidth
            spacing: mainWindow.compactLayout ? 8 : 10
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: headerRow.bottom
            anchors.topMargin: mainWindow.compactLayout ? 20 : 28
            anchors.bottom: actionsRow.top
            anchors.bottomMargin: mainWindow.compactLayout ? 20 : 28

            Item {
                id: heroSection
                width: parent.width
                height: mainWindow.heroSectionHeight

                Rectangle {
                    id: avatarContainer
                    width: mainWindow.heroSize
                    height: mainWindow.heroSize
                    radius: width / 2
                    color: mainWindow.theme.minimumAccent
                    anchors.centerIn: parent
                    z: 2

                    Text {
                        anchors.centerIn: parent
                        text: mainWindow.fallbackInitial()
                        font.pixelSize: 34
                        font.family: geologicaFont.name
                        color: mainWindow.theme.accent
                        // Show the initial whenever no avatar image is actually
                        // displayed — empty source, a load error (e.g. an SVG
                        // fallback that doesn't render on Android) or still
                        // loading. Keying off source alone left the circle empty.
                        visible: avatarImg.status !== Image.Ready
                    }

                    Image {
                        id: avatarImg
                        anchors.fill: parent
                        visible: false
                        source: mainWindow.resolvedAvatarSource()
                        cache: false
                        fillMode: Image.PreserveAspectCrop
                        mipmap: true
                        smooth: true
                        antialiasing: true
                        layer.enabled: true
                        layer.smooth: true
                    }

                    Connections {
                        target: mainWindow.connectionBridge

                        function onContactAvatarChanged(updatedServerId, updatedPubKeyFingerprint) {
                            if (updatedServerId === mainWindow.contactServer && updatedPubKeyFingerprint === mainWindow.contactPubKey)
                                avatarImg.source = mainWindow.resolvedAvatarSource()
                        }
                    }

                    Rectangle {
                        id: avatarMask
                        anchors.fill: parent
                        radius: avatarContainer.radius
                        visible: false
                        layer.enabled: true
                        layer.smooth: true
                    }

                    MultiEffect {
                        anchors.fill: parent
                        source: avatarImg
                        maskEnabled: true
                        maskSource: avatarMask
                        visible: avatarImg.status === Image.Ready
                        smooth: true
                        antialiasing: true
                        maskSpreadAtMin: 1.0
                        maskThresholdMin: 0.6
                    }
                }
            }

            StyledName {
                width: parent.width
                firstName: mainWindow.firstName
                lastName: mainWindow.lastName
                styleCss: mainWindow.contactNameStyle
                fontFamily: geologicaFont.name
                fallbackColor: mainWindow.theme.accent
                pixelSize: mainWindow.compactLayout ? 20 : 24
                fontWeight: Font.Medium
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                anchors.topMargin: 20
                anchors.bottomMargin: 20
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: mainWindow.mainCallInfo
                font.pixelSize: mainWindow.compactLayout ? 18 : 20
                font.family: geologicaFont.name
                font.weight: Font.Bold
                font.letterSpacing: 1.5
                color: mainWindow.theme.accent
            }

            Rectangle {
                width: parent.width
                height: visible ? incomingCallText2.implicitHeight + 20 : 0
                visible: mainWindow.descriptionCallVisible
                color: mainWindow.theme.minimumAccent
                radius: 8

                Text {
                    id: incomingCallText2
                    anchors.centerIn: parent
                    width: parent.width - 20
                    text: mainWindow.descriptionCall
                    font.pixelSize: 12
                    font.family: geologicaFont.name
                    font.weight: Font.Medium
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    color: mainWindow.theme.halfAccent
                }
            }

            Text {
                width: parent.width
                text: mainWindow.mainTitleText
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                font.family: geologicaFont.name
                font.weight: Font.Medium
                color: mainWindow.theme.halfAccent
            }
        }

        Row {
            id: actionsRow
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            height: mainWindow.bottomActionsHeight
            spacing: mainWindow.speakerButtonVisible ? (mainWindow.compactLayout ? 24 : 32) : mainWindow.buttonSpacing

            BottomButton {
                id: discardButton
                width: mainWindow.buttonSize
                height: mainWindow.buttonSize
                anchors.verticalCenter: parent.verticalCenter
                rotation: 135
                icon.source: "resources/call.svg"
                icon.color: mainWindow.theme.redAccent
                icon.width: 40
                icon.height: 40
                onClicked: mainWindow.closeCallView()
            }

            BottomButton {
                id: speakerButton
                width: mainWindow.buttonSize
                height: mainWindow.buttonSize
                anchors.verticalCenter: parent.verticalCenter
                visible: mainWindow.speakerButtonVisible
                icon.source: "resources/speaker.svg"
                icon.color: mainWindow.theme.accent
                icon.width: 40
                icon.height: 40
                opacity: AndroidSystemUi.speakerphoneOn ? 1.0 : 0.7
                // background: Rectangle { color: "transparent" }
                onClicked: AndroidSystemUi.toggleSpeakerphone()
            }

            BottomButton {
                id: acceptButton
                width: mainWindow.buttonSize
                height: mainWindow.buttonSize
                anchors.verticalCenter: parent.verticalCenter
                visible: mainWindow.type === "incoming" && mainWindow.status !== "active"
                icon.source: "resources/call.svg"
                icon.color: mainWindow.theme.accent
                icon.width: 40
                icon.height: 40
                onClicked: mainWindow.acceptCallView()
            }
        }
    }
}