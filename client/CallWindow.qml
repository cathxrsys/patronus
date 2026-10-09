import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls.Material
import QtQuick.Effects
import QtQuick.Shapes
import QtQml
import QtMultimedia

Window {
    id: mainWindow

    FontLoader {
        id: geologicaFont
        source: "resources/fonts/geologica.ttf"
    }

    property string themeId: Settings.getTextSetting("themeKey", themeCatalog.defaultTheme)

    ThemeCatalog {
        id: themeCatalog
    }

    // active theme object
    property var theme: themeLoader.item

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
    }

    SoundEffect {
        id: requestSound
        source: "resources/sounds/incoming_call_Default.wav"
        loops: SoundEffect.Infinite
    }

    SoundEffect {
        id: discardSound
        source: "resources/sounds/call_discard.wav"
        loops: 1
    }

    SoundEffect {
        id: acceptSound
        source: "resources/sounds/accept_call.wav"
        loops: 1
    }

    Timer {
        id: startupTimer
        interval: 1
        running: true
        repeat: false
        onTriggered: {
            if (mainWindow.type === "outgoing" && mainWindow.status === "calling") {
                callingSound.play()
            }
            if (mainWindow.type === "incoming" && mainWindow.status === "request") {
                requestSound.play()
            }
        }
    }

    Material.theme: Material.Dark
    Material.accent: theme.accent

    property string type: "incoming" // incoming | outgoing | error
    property string status: "calling" // status : active calling, request
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
    property real voiceLevel: 1.0
    property string dh_pub: ""
    property string dh_id: ""
    property real callId: 0

    Connections { // !!!!!!
        target: connections

        function onCallAcceptReceived() {
            callingSound.stop()
            requestSound.stop()
            acceptSound.play()
            mainWindow.status = "active"
            mainWindow.mainCallInfo = connections.getCallKeysFingerprint()
            mainWindow.descriptionCallVisible = true
        }
    }

    flags: Qt.FramelessWindowHint
    property bool isWindowMaximized: false
    property real restoreX: 0
    property real restoreY: 0
    property real restoreWidth: 640
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

    width: 640
    height: 640
    minimumHeight: 640
    minimumWidth: 640
    Component.onCompleted: centerOnCurrentScreen()

    Connections { // !!!!
        target: connections

        function onCallDiscardReceived(serverId, contactPubKey, callId, reason) {
            if (mainWindow.contactServer === serverId
                    && mainWindow.contactPubKey === contactPubKey
                    && mainWindow.callId === callId) {
                callingSound.stop()
                requestSound.stop()
                discardSound.play()

                // The callee was busy on another call. Show "Contact is busy" and
                // leave teardown to CallManager, which keeps the window up
                // briefly; do not discard() here, to avoid a double teardown.
                if (reason === "busy") {
                    mainWindow.mainCallInfo = qsTr("Contact is busy")
                    return
                }

                if (mainWindow.type === "error") {
                    mainWindow.close()
                    return
                } else if (mainWindow.type === "incoming") {
                    callManager.discard(mainWindow.contactServer, mainWindow.contactPubKey, callId, false, mainWindow.status)
                } else if (mainWindow.type === "outgoing") {
                    callManager.discard(mainWindow.contactServer, mainWindow.contactPubKey, callId, true, mainWindow.status)
                }
            }
        }

        // The caller cancelled (missed call). CallManager closes the window and
        // records the missed call in C++; here we just stop the ringing.
        function onCallCancelReceived(serverId, contactPubKey, callId) {
            if (mainWindow.contactServer === serverId
                    && mainWindow.contactPubKey === contactPubKey
                    && mainWindow.callId === callId) {
                callingSound.stop()
                requestSound.stop()
                discardSound.play()
            }
        }
    }

    color: theme.background

    visible: true
    title: qsTr("Call")
    onVisibilityChanged: {
        isWindowMaximized = visibility === Window.Maximized
    }

    Rectangle { // top panel
        id: topPanel
        width: parent.width
        height: 25

        z: 300

        color: theme.background

        RowLayout { 
            id: topPanel2
            width: parent.width
            height: 25

            Rectangle {
                Layout.fillHeight: true
                color: "transparent"

                Logo {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 13
                    height: 14
                    accentColor: theme.accent
                    MouseArea {
                        anchors.fill: parent
                        onPressed: {
                            mainWindow.startSystemMove()
                        }
                    }
                }
                Text { // title
                    text: "patronus"
                    color: theme.accent
                    font.pixelSize: 11
                    font.family: "Inter"
                    font.weight: Font.Bold
                    anchors.left: parent.left
                    anchors.leftMargin: 30
                    anchors.verticalCenter: parent.verticalCenter
                    MouseArea {
                        anchors.fill: parent
                        onPressed: {
                            mainWindow.startSystemMove()
                        }
                    }
                }
            }

            MouseArea {
                Layout.fillWidth: true
                Layout.fillHeight: true
                
                onPressed: {
                    mainWindow.startSystemMove()
                }
            }

            RowLayout {
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

                PanelButton {
                    Layout.alignment: Qt.AlignRight
                    enabled: false
                    icon.source: mainWindow.isWindowMaximized ? "resources/restore.svg" : "resources/maximize.svg"
                    icon.color: enabled ? theme.accent : theme.lowAccent
                    onClicked: {
                        mainWindow.toggleMaximize()
                    }
                }

                PanelButton {
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

    Rectangle {
        id: contentArea
        anchors.top: topPanel.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        color: theme.background

        Rectangle { // avatar in the center of the window
            id: avatarContainer2
            height: 140
            width: 140

            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: parent.height / 8

            radius: width / 2
            color: theme.minimumAccent

            layer.enabled: true

            z: 101

            Text {
                id: avatarLetter2
                anchors.centerIn: parent
                text: mainWindow.firstName[0]
                font.pixelSize: 34
                font.family: geologicaFont.name
                color: theme.accent

                visible: Account.hasContactAvatar(mainWindow.contactServer, mainWindow.contactPubKey) === false

                z: 101
            }

            Image { // avatar from file
                id: avatarImg2

                anchors.fill: parent
                visible: false

                source: Account.getContactAvatarSource(mainWindow.contactServer, mainWindow.contactPubKey)
                
                fillMode: Image.PreserveAspectCrop

                mipmap: true
                smooth: true
                antialiasing: true

                layer.enabled: true
                layer.smooth: true

                z: 101
            }

            Connections {
                target: connections

                function onContactAvatarChanged(updatedServerId, updatedPubKeyFingerprint) {
                    if (updatedServerId === mainWindow.contactServer && updatedPubKeyFingerprint === mainWindow.contactPubKey) {
                        avatarImg2.source = Account.getContactAvatarSource(mainWindow.contactServer, mainWindow.contactPubKey) + "?t=" + Date.now();
                    }
                }
            }

            Rectangle { // mask for avatar
                id: mask2
                anchors.fill: parent
                radius: avatarContainer2.radius
                visible: false
                layer.enabled: true
                layer.smooth: true

                z: 101
            }

            MultiEffect { // effect for avatar circle
                id: avatarMaskEffect2
                anchors.fill: parent
                source: avatarImg2
                maskEnabled: true
                maskSource: mask2
                visible: Account.hasContactAvatar(mainWindow.contactServer, mainWindow.contactPubKey)
                smooth: true
                antialiasing: true

                maskSpreadAtMin: 1.0
                maskThresholdMin: 0.6

                z: 101
            }
        } // avatar in the center of the window

        StyledName {
            id: styledNameEl
            anchors.top: avatarContainer2.bottom
            anchors.topMargin: 20
            anchors.bottomMargin: 20
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 80

            firstName: mainWindow.firstName
            lastName: mainWindow.lastName
            styleCss: mainWindow.contactNameStyle
            fontFamily: geologicaFont.name
            fallbackColor: theme.accent
            pixelSize: 22
            fontWeight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }

        Text {
            id: incomingCallText
            anchors.top: styledNameEl.bottom
            anchors.topMargin: 8
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 80

            text: mainCallInfo
            elide: Text.ElideMiddle
            horizontalAlignment: Text.AlignHCenter

            font.pixelSize: 24
            font.family: geologicaFont.name
            font.weight: Font.Bold

            font.letterSpacing: 1.5

            color: theme.accent
        }

        Rectangle {
            id: secureCallContainer

            anchors.top: incomingCallText.bottom
            anchors.topMargin: 20
            anchors.horizontalCenter: parent.horizontalCenter

            color: theme.minimumAccent
            radius: 5

            visible: descriptionCallVisible

            width: incomingCallText2.width + 20
            height: incomingCallText2.implicitHeight + 20

            Text {
                id: incomingCallText2
                text: descriptionCall
                
                font.pixelSize: 12
                font.family: geologicaFont.name
                font.weight: Font.Medium
                wrapMode: Text.WordWrap
                width: 300

                anchors.centerIn: parent
                color: theme.halfAccent
            }
        }

        Text {
            id: incomingCallText3
            anchors.top: secureCallContainer.bottom
            anchors.topMargin: 20
            anchors.horizontalCenter: parent.horizontalCenter

            text: mainTitleText

            font.pixelSize: 12
            font.family: geologicaFont.name
            font.weight: Font.Medium

            color: theme.halfAccent
        }

        RowLayout {
            id: callButtonsRow
            anchors.top: incomingCallText3.bottom
            anchors.topMargin: 80
            anchors.horizontalCenter: parent.horizontalCenter

            spacing: 80

            BottomButton {
                id: discardButton

                height: 100
                width: 100

                rotation: 135

                icon.source: "resources/call.svg"
                icon.color: theme.redAccent

                icon.width: 40
                icon.height: 40

                onClicked: {
                    callingSound.stop()
                    requestSound.stop()
                    discardSound.play()
                    if (mainWindow.type === "error") {
                        mainWindow.close()
                        return
                    } else if (mainWindow.type === "incoming") {
                        callManager.discard(
                            mainWindow.contactServer,
                            mainWindow.contactPubKey,
                            mainWindow.callId,
                            false,
                            mainWindow.status
                        )
                    } else if (mainWindow.type === "outgoing") {
                        // The caller hanging up before the call is answered is a
                        // missed call (call_cancel); ending an active call is a
                        // normal hang up (call_discard).
                        if (mainWindow.status === "active") {
                            callManager.discard(
                                mainWindow.contactServer,
                                mainWindow.contactPubKey,
                                mainWindow.callId,
                                true,
                                mainWindow.status
                            )
                        } else {
                            callManager.cancel(
                                mainWindow.contactServer,
                                mainWindow.contactPubKey,
                                mainWindow.callId,
                                true,
                                mainWindow.status
                            )
                        }
                    }
                }

                PropertyAnimation {
                    id: discardWobbleAnim1
                    target: discardButton
                    property: "rotation"
                    from: 135
                    to: 141
                    duration: 60
                    easing.type: Easing.InOutQuad
                }
                PropertyAnimation {
                    id: discardWobbleAnim2
                    target: discardButton
                    property: "rotation"
                    from: 141
                    to: 129
                    duration: 120
                    easing.type: Easing.InOutQuad
                }
                PropertyAnimation {
                    id: discardWobbleAnim3
                    target: discardButton
                    property: "rotation"
                    from: 129
                    to: 135
                    duration: 60
                    easing.type: Easing.InOutQuad
                }
                SequentialAnimation {
                    id: discardWobbleAnim
                    running: false
                    loops: 2
                    onStopped: discardButton.rotation = 135
                    animations: [
                        discardWobbleAnim1,
                        discardWobbleAnim2,
                        discardWobbleAnim3
                    ]
                }
                HoverHandler {
                    id: discardHoverHandler
                    onHoveredChanged: {
                        if (hovered) {
                            discardWobbleAnim.running = true
                        } else {
                            discardWobbleAnim.running = false
                            discardButton.rotation = 135
                        }
                    }
                }
            } // callButton

            BottomButton {
                id: acceptButton

                height: 100
                width: 100

                visible: mainWindow.type === "incoming" ? (mainWindow.status === "active" ? false : true) : false

                icon.source: "resources/call.svg"
                icon.color: theme.accent

                icon.width: 40
                icon.height: 40

                onClicked: {
                    callingSound.stop()
                    requestSound.stop()
                    acceptSound.play()

                    mainWindow.status = "active"
                    
                    mainWindow.descriptionCallVisible = true

                    callManager.accept(
                        mainWindow.contactServer,
                        mainWindow.contactPubKey,
                        mainWindow.dh_pub,
                        mainWindow.dh_id,
                        mainWindow.callId
                    )

                    mainWindow.mainCallInfo = connections.getCallKeysFingerprint()
                }

                PropertyAnimation {
                    id: acceptWobbleAnim1
                    target: acceptButton
                    property: "rotation"
                    from: 0
                    to: 6
                    duration: 60
                    easing.type: Easing.InOutQuad
                }
                PropertyAnimation {
                    id: acceptWobbleAnim2
                    target: acceptButton
                    property: "rotation"
                    from: 6
                    to: -6
                    duration: 120
                    easing.type: Easing.InOutQuad
                }
                PropertyAnimation {
                    id: acceptWobbleAnim3
                    target: acceptButton
                    property: "rotation"
                    from: -6
                    to: 0
                    duration: 60
                    easing.type: Easing.InOutQuad
                }
                SequentialAnimation {
                    id: acceptWobbleAnim
                    running: false
                    loops: 2
                    onStopped: acceptButton.rotation = 0
                    animations: [
                        acceptWobbleAnim1,
                        acceptWobbleAnim2,
                        acceptWobbleAnim3
                    ]
                }
                HoverHandler {
                    id: acceptHoverHandler
                    onHoveredChanged: {
                        if (hovered) {
                            acceptWobbleAnim.running = true
                        } else {
                            acceptWobbleAnim.running = false
                            acceptButton.rotation = 0
                        }
                    }
                }
            } // callButton
        }
    }
}