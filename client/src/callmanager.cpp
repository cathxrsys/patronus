#include "callmanager.h"

#include <QDateTime>
#include <QQmlContext>
#include <QScreen>
#include <QTimer>

#include "androidsystemui.h"
#include "audiodevicemanager.h"
#include "contactsmodel.h"
#include "databasemanager.h"
#include "messagemodel.h"

namespace {
void secureClearCallKeys(CoreCrypto::SessionKeys& sessionKeys) {
  CoreCrypto::secure_memory_zero(sessionKeys.send_key.data(),
                                 sessionKeys.send_key.size());
  CoreCrypto::secure_memory_zero(sessionKeys.receive_key.data(),
                                 sessionKeys.receive_key.size());
}
}  // namespace

bool CallManager::useEmbeddedCallUi() const {
#ifdef Q_OS_ANDROID
  return true;
#else
  return false;
#endif
}

QQuickWindow* CallManager::mainApplicationWindow() const {
  if (!m_engine) {
    return nullptr;
  }

  const auto rootObjects = m_engine->rootObjects();
  for (QObject* rootObject : rootObjects) {
    if (auto* window = qobject_cast<QQuickWindow*>(rootObject)) {
      return window;
    }
  }

  return nullptr;
}

void CallManager::setPreferredDesktopScreen(QScreen* screen) {
#ifdef Q_OS_ANDROID
  Q_UNUSED(screen);
#else
  m_preferredDesktopScreen = screen;
#endif
}

void CallManager::destroyCallUi() {
  // No call UI on screen anymore: allow the connection to drop when backgrounded.
  if (m_connectionManager) {
    m_connectionManager->setCallActive(false);
  }

  if (!m_callUi) {
    return;
  }

  if (auto* window = qobject_cast<QQuickWindow*>(m_callUi)) {
    window->close();
  }

  if (auto* item = qobject_cast<QQuickItem*>(m_callUi)) {
    item->setVisible(false);
    item->setParentItem(nullptr);
  }

  m_callUi->deleteLater();
  m_callUi = nullptr;
}

QObject* CallManager::createCallUi() {
  destroyCallUi();

  if (!m_engine) {
    qWarning() << "[CallManager] QML engine is not available for call UI";
    return nullptr;
  }

  const QUrl componentUrl(
      useEmbeddedCallUi()
          ? QStringLiteral("qrc:/qt/qml/client/MobileCallView.qml")
          : QStringLiteral("qrc:/qt/qml/client/CallWindow.qml"));
  QQmlComponent component(m_engine, componentUrl);
  if (component.isError()) {
    qWarning() << "[CallManager] Failed to load call UI component"
               << component.errors();
    return nullptr;
  }

  QObject* obj = component.create(m_engine->rootContext());
  if (!obj) {
    qWarning() << "[CallManager] Failed to create call UI"
               << component.errors();
    return nullptr;
  }

  if (useEmbeddedCallUi()) {
    auto* item = qobject_cast<QQuickItem*>(obj);
    auto* mainWindow = mainApplicationWindow();
    if (!item || !mainWindow || !mainWindow->contentItem()) {
      qWarning()
          << "[CallManager] Embedded call UI requires a main QQuickWindow";
      obj->deleteLater();
      return nullptr;
    }

    item->setParent(mainWindow->contentItem());
    item->setParentItem(mainWindow->contentItem());
    item->setZ(10'000);
    item->setVisible(true);
    item->forceActiveFocus();
  } else {
    auto* window = qobject_cast<QQuickWindow*>(obj);
    if (!window) {
      qWarning() << "[CallManager] Desktop call UI is not a QQuickWindow";
      obj->deleteLater();
      return nullptr;
    }

    QScreen* targetScreen =
        m_preferredDesktopScreen ? m_preferredDesktopScreen.data() : nullptr;
    if (!targetScreen) {
      if (auto* mainWindow = mainApplicationWindow()) {
        targetScreen = mainWindow->screen();
      }
    }
    if (targetScreen) {
      window->setScreen(targetScreen);
      const QRect availableGeometry = targetScreen->availableGeometry();
      window->setX(availableGeometry.center().x() - window->width() / 2);
      window->setY(availableGeometry.center().y() - window->height() / 2);
    }

    window->show();
    window->requestActivate();
  }

  m_callUi = obj;
  // A call is on screen now (outgoing/ringing/active); keep the connection alive
  // even if the app is backgrounded so the call isn't dropped.
  if (m_connectionManager) {
    m_connectionManager->setCallActive(true);
  }
  return obj;
}

CallManager::CallManager(QQmlApplicationEngine* engine, QObject* parent,
                         ConnectionManager* connectionManager,
                         DatabaseManager* db, MessageModel* messageModel,
                         ContactsModel* contactsModel,
                         AudioDeviceManager* audioDeviceManager)
    : QObject(parent),
      m_db(db),
      m_messageModel(messageModel),
      m_contactsModel(contactsModel),
      m_engine(engine),
      m_connectionManager(connectionManager),
      m_recorder(new OpusRecorder(audioDeviceManager, this)),
      m_player(new OpusPlayer(GITTER_DEFAULT_BUFFER_MS, this)) {
  connect(m_recorder, &OpusRecorder::frameEncoded, this,
          &CallManager::onAudioFrameEncoded);
  connect(m_recorder, &OpusRecorder::errorOccurred, this,
          &CallManager::onRecorderError);
  connect(m_player, &OpusPlayer::errorOccurred, this,
          &CallManager::onPlayerError);
  connect(m_connectionManager, &ConnectionManager::audioFrameReceived, this,
          &CallManager::onAudioFrameReceived);
  connect(m_connectionManager, &ConnectionManager::callAcceptReceived, this,
          &CallManager::onCallAccepted);
    connect(m_connectionManager, &ConnectionManager::callDiscardReceived, this,
      &CallManager::onCallDiscardReceived);
  connect(m_connectionManager, &ConnectionManager::callCancelReceived, this,
          &CallManager::onCallCancelReceived);
  connect(m_connectionManager, &ConnectionManager::offlineSyncStarted, this,
          &CallManager::onOfflineSyncStarted);
  connect(m_connectionManager, &ConnectionManager::offlineSyncEnded, this,
          &CallManager::onOfflineSyncEnded);
  connect(m_player, &OpusPlayer::voiceLevelChanged, this,
          &CallManager::onRemoteVoiceLevelChanged);

  connect(m_connectionManager, &ConnectionManager::connectionStatusChanged,
          this,
          [this](const QString& serverId,
                 ConnectionManager::ConnectionStatus status) {
            const bool available =
                status == ConnectionManager::ConnectionStatus::Connected ||
                status == ConnectionManager::ConnectionStatus::Connecting ||
                status == ConnectionManager::ConnectionStatus::Reconnecting;
            onConnectionAvailabilityChanged(serverId, available);
          });
  connect(m_connectionManager, &ConnectionManager::authChanged, this,
          [this](const QString& serverId, bool success) {
            onConnectionAvailabilityChanged(serverId, success);
          });

  // The user tapped Accept/Decline on the call notification while a call is
  // already on screen — apply it immediately (the queued-then-applied path in
  // presentCallRequest only covers actions chosen before the call appears).
  if (auto* androidSystemUi = AndroidSystemUi::instance()) {
    connect(androidSystemUi, &AndroidSystemUi::notificationCallActionRequested,
            this, &CallManager::applyPendingCallAction);
  }
}

CallManager::~CallManager() {
  stopAudioStream();
  destroyCallUi();
}

void CallManager::startCall(const QString& contactFirstName,
                            const QString& contactAvatar,
                            const QString& contactPubKey,
                            const QString& contactServer) {
  QObject* callUi = createCallUi();

  if (callUi) {
    callUi->setProperty("firstName", contactFirstName);
    callUi->setProperty("avatarSource", contactAvatar);
    callUi->setProperty("pubKey", contactPubKey);

    callUi->setProperty("contactPubKey", contactPubKey);
    callUi->setProperty("contactServer", contactServer);

    callUi->setProperty("type", "outgoing");
    callUi->setProperty("mainTitleText", tr("Outgoing Call"));
    callUi->setProperty("descriptionCallVisible", false);
    callUi->setProperty("mainCallInfo", tr("Calling..."));
    callUi->setProperty("status", "calling");

    quint64 callId = m_messageModel ? m_messageModel->uniqId() : 0;
    callUi->setProperty("callId", static_cast<quint64>(callId));
    setCurrentCallIdentity(contactServer, contactPubKey, callId);

    if (auto* androidSystemUi = AndroidSystemUi::instance()) {
      androidSystemUi->setCallProximityActive(true);
    }

    m_connectionManager->e2eCallRequest(contactServer, contactPubKey, callId);
  }
}

void CallManager::showCallError(const QString& contactFirstName,
                                const QString& contactAvatar,
                                const QString& contactPubKey,
                                const QString& contactServer,
                                const QString& errorMessage) {
  QObject* callUi = createCallUi();

  if (callUi) {
    callUi->setProperty("firstName", contactFirstName);
    callUi->setProperty("avatarSource", contactAvatar);
    callUi->setProperty("pubKey", contactPubKey);

    callUi->setProperty("contactServer", contactServer);
    callUi->setProperty("contactPubKey", contactPubKey);

    callUi->setProperty("descriptionCall", errorMessage);
    callUi->setProperty("mainCallInfo", tr("Call Error"));
    callUi->setProperty("mainTitleText", tr("Outgoing Call"));
    callUi->setProperty("type", "error");
    callUi->setProperty("status", "error");

    if (auto* androidSystemUi = AndroidSystemUi::instance()) {
      androidSystemUi->setCallProximityActive(false);
    }
  }
}

void CallManager::showCallRequest(const QString& contactFirstName,
                                  const QString& contactAvatar,
                                  const QString& contactServer,
                                  const QString& contactPubKey,
                                  const QString& dh_pub, const QString& id,
                                  const uint64_t callId) {
  const PendingIncomingCall call{contactFirstName, contactAvatar,
                                 contactServer,    contactPubKey,
                                 dh_pub,           id,
                                 callId};

  // While replaying the offline message queue, hold the incoming call back
  // instead of ringing immediately: a call_cancel queued right behind the
  // call_request can then suppress it before it is ever shown
  // (onCallCancelReceived removes it from the buffer). Buffered calls that
  // survive are shown once the sync finishes. Live calls are never buffered.
  if (m_syncingServers.contains(contactServer)) {
    m_bufferedIncomingCalls.append(call);
    return;
  }

  presentCallRequest(call);
}

void CallManager::presentCallRequest(const PendingIncomingCall& call) {
  // Already engaged in another call (outgoing/ringing/active): auto-decline this
  // one as busy instead of ringing. The server stays a dumb relay — "busy" is a
  // purely client-side, online-only decision. We tell the caller (reason=busy so
  // their UI can show "Contact is busy") and log a missed call locally, but never
  // touch the in-progress call's state: e2eCallDiscard and addCallLogMessage act
  // only on the incoming call's identity passed here.
  if (m_currentCallId != 0) {
    // This incoming call (not the in-progress one) is what the OS notification
    // ringer, if any, is currently ringing for — dismiss it here or it keeps
    // ringing/vibrating forever after we silently auto-decline as busy.
    if (auto* androidSystemUi = AndroidSystemUi::instance()) {
      androidSystemUi->cancelIncomingCallNotification();
    }
    m_connectionManager->e2eCallDiscard(call.contactServer, call.contactPubKey,
                                        call.callId, QStringLiteral("busy"));
    addCallLogMessage(call.contactServer, call.contactPubKey, call.callId,
                      QStringLiteral("Missed Call"), false);
    return;
  }

  QObject* callUi = createCallUi();

  if (callUi) {
    // A call surfaced from an FCM push is already being rung by the system
    // notification ringer; tell the call UI to suppress its own ringtone so the
    // two never overlap. Must be set before "status" (which triggers ringing).
    bool externalRing = false;
    if (auto* androidSystemUi = AndroidSystemUi::instance()) {
      externalRing = androidSystemUi->hasActiveIncomingCallNotification();
    }
    callUi->setProperty("externalRing", externalRing);
    m_incomingNotificationPending = externalRing;

    callUi->setProperty("firstName", call.firstName);
    callUi->setProperty("avatarSource", call.avatarSource);
    callUi->setProperty("pubKey", call.contactPubKey);

    callUi->setProperty("contactServer", call.contactServer);
    callUi->setProperty("contactPubKey", call.contactPubKey);

    callUi->setProperty("dh_pub", call.dh_pub);
    callUi->setProperty("dh_id", call.id);

    callUi->setProperty("type", "incoming");
    callUi->setProperty("mainTitleText", tr("Incoming Call"));
    callUi->setProperty("descriptionCallVisible", false);
    callUi->setProperty("mainCallInfo", tr("Incoming Call"));
    callUi->setProperty("status", "request");

    callUi->setProperty("callId", static_cast<quint64>(call.callId));
    setCurrentCallIdentity(call.contactServer, call.contactPubKey, call.callId);

    if (auto* androidSystemUi = AndroidSystemUi::instance()) {
      androidSystemUi->setCallProximityActive(false);
      // If this call woke us from an FCM push, relabel that generic notification
      // with who is actually calling (no-op for live foreground calls).
      androidSystemUi->updateIncomingCallNotification(call.firstName);
    }

    // If the user already tapped Accept/Decline on the notification before the
    // call appeared, perform it now that it is on screen.
    applyPendingCallAction();
  }
}

void CallManager::applyPendingCallAction() {
  // Only meaningful once a call is on screen. If none is yet (e.g. still
  // connecting/buffering), leave the action queued; presentCallRequest applies
  // it when the call appears.
  if (!m_callUi) {
    return;
  }

  auto* androidSystemUi = AndroidSystemUi::instance();
  if (!androidSystemUi) {
    return;
  }

  const QString action = androidSystemUi->consumePendingCallAction();
  if (action == QStringLiteral("accept")) {
    QMetaObject::invokeMethod(m_callUi, "acceptCallView");
  } else if (action == QStringLiteral("decline")) {
    QMetaObject::invokeMethod(m_callUi, "closeCallView");
  }
}

void CallManager::discard(const QString& contactServer,
                          const QString& contactPubKey, const uint64_t callId,
                          bool isOwnCallRequest, const QString& type) {
  stopAudioStream();

  if (!contactServer.isEmpty() && !contactPubKey.isEmpty()) {
    m_connectionManager->e2eCallDiscard(contactServer, contactPubKey, callId);
  } else if (!m_currentContactServer.isEmpty() &&
             !m_currentContactPubKey.isEmpty()) {
    m_connectionManager->e2eCallDiscard(m_currentContactServer,
                                        m_currentContactPubKey, callId);
  }

  resetCallState(true);

  if (type != "active") {
    addCallLogMessage(contactServer, contactPubKey, callId,
                      QStringLiteral("Discarded Call"), isOwnCallRequest);
  }
}

void CallManager::cancel(const QString& contactServer,
                         const QString& contactPubKey, const uint64_t callId,
                         bool isOwnCallRequest, const QString& type) {
  stopAudioStream();

  if (!contactServer.isEmpty() && !contactPubKey.isEmpty()) {
    m_connectionManager->e2eCallCancel(contactServer, contactPubKey, callId);
  } else if (!m_currentContactServer.isEmpty() &&
             !m_currentContactPubKey.isEmpty()) {
    m_connectionManager->e2eCallCancel(m_currentContactServer,
                                       m_currentContactPubKey, callId);
  }

  resetCallState(true);

  if (type != "active") {
    addCallLogMessage(contactServer, contactPubKey, callId,
                      QStringLiteral("Missed Call"), isOwnCallRequest);
  }
}

void CallManager::addCallLogMessage(const QString& contactServer,
                                    const QString& contactPubKey,
                                    quint64 callId, const QString& text,
                                    bool isOwnCallRequest) {
  const quint64 timestamp =
      static_cast<quint64>(QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
  const QString t = QTime::currentTime().toString("hh:mm");

  if (m_messageModel != nullptr &&
      m_messageModel->isCurrentChat(contactServer, contactPubKey)) {
    m_messageModel->addMessage(static_cast<quint64>(callId), text, t,
                               isOwnCallRequest, true, false, true, true, true,
                               true);
  }

  if (m_db != nullptr) {
    m_db->addMessage(static_cast<quint64>(callId), contactServer, contactPubKey,
                     text, timestamp, isOwnCallRequest, true, true, true, true,
                     true);
  }
  if (m_contactsModel != nullptr) {
    m_contactsModel->setLastMessageAt(contactServer, contactPubKey, text,
                                      timestamp);
  }
}

void CallManager::accept(const QString& contactServer,
                         const QString& contactPubKey, const QString& dh_pub,
                         const QString& id, const uint64_t callId) {
  if (m_callUi) {
    if (auto* androidSystemUi = AndroidSystemUi::instance()) {
      androidSystemUi->setCallProximityActive(false);
      // Mark answered before dismissing the notification so the activity is not
      // sent to the background while we set up the (now active) call.
      androidSystemUi->markCallAnswered();
      androidSystemUi->cancelIncomingCallNotification();
    }
    m_incomingNotificationPending = false;

    std::string dh_pub_str = dh_pub.toStdString();
    std::vector<uint8_t> dh_pub_vec = coreutils::hex_to_bytes(dh_pub_str);
    std::vector<uint8_t> dh_id_vec = coreutils::hex_to_bytes(id.toStdString());

    CoreCrypto::DHKeyPair ephemeralKeys = CoreCrypto::generate_dh_keypair();

    CoreCrypto::SessionKeys sessionKeys = CoreCrypto::derive_dh_keys(
        true, ephemeralKeys.pubkey, ephemeralKeys.privkey, dh_pub_vec);

    m_connectionManager->setCallSessionState(sessionKeys, contactServer,
                                             contactPubKey);
    m_sessionKeys = sessionKeys;

    setCurrentCallIdentity(contactServer, contactPubKey,
                 static_cast<quint64>(callId));

    m_connectionManager->e2eCallAccept(contactServer, contactPubKey,
                                       ephemeralKeys.pubkey, dh_id_vec, callId);

    const quint64 timestamp = static_cast<quint64>(
        QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
    QString t = QTime::currentTime().toString("hh:mm");
    m_activeCallStartEpoch = timestamp;

    if (m_messageModel != nullptr &&
        m_messageModel->isCurrentChat(m_currentContactServer,
                                      m_currentContactPubKey)) {
      m_messageModel->addMessage(static_cast<quint64>(callId),
                                 QStringLiteral("Accepted Call"), t, false,
                                 true, false, true, true, true, true);
    }

    if (m_db != nullptr) {
      m_db->addMessage(static_cast<quint64>(callId), m_currentContactServer,
                       m_currentContactPubKey,
                       QStringLiteral("Accepted Call"), timestamp, false,
                       true, true, true, true, true);
    }
    if (m_contactsModel != nullptr) {
      m_contactsModel->setLastMessageAt(m_currentContactServer,
                                        m_currentContactPubKey,
                                        QStringLiteral("Accepted Call"),
                                        timestamp);
    }
    m_audioSendCounter = 0;
    m_audioRecvCounter = 0;

    startAudioStream();
  }
}

void CallManager::onCallAccepted() {
  if (auto* androidSystemUi = AndroidSystemUi::instance()) {
    androidSystemUi->setCallProximityActive(false);
  }

  m_sessionKeys = m_connectionManager->callSessionKeys();
  const quint64 callId =
      m_callUi ? m_callUi->property("callId").toULongLong() : m_currentCallId;
  setCurrentCallIdentity(m_connectionManager->currentCallServer(),
                         m_connectionManager->currentCallContactPubKey(),
                         callId);
  m_activeCallStartEpoch = static_cast<quint64>(
      QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
  m_audioSendCounter = 0;
  m_audioRecvCounter = 0;

  if (m_callUi) {
    m_callUi->setProperty("status", "active");
    m_callUi->setProperty("mainCallInfo",
                          m_connectionManager->getCallKeysFingerprint());
    m_callUi->setProperty("mainTitleText", tr("Outgoing Call"));
  }

  startAudioStream();
}

void CallManager::startAudioStream() {
  if (m_audioActive) return;

  m_audioActive = true;
  if (auto* androidSystemUi = AndroidSystemUi::instance()) {
    androidSystemUi->setCallAudioActive(true);
  }
  m_recorder->startRecording();
  m_player->start();

  // No QAudioSink refresh needed on Android: VoiceCallAudioPlayer uses
  // AudioTrack with USAGE_VOICE_COMMUNICATION and is unaffected by the
  // QMediaDevices default-device enumeration race.
}

void CallManager::stopAudioStream() {
  if (!m_audioActive) return;

  m_audioActive = false;
  m_recorder->stopRecording();
  m_player->stop();
  if (auto* androidSystemUi = AndroidSystemUi::instance()) {
    androidSystemUi->setCallAudioActive(false);
  }
}

void CallManager::setJitterBufferMs(int ms) {
  if (m_jitterBufferMs != ms) {
    m_jitterBufferMs = ms;
    m_player->setJitterBufferSize(ms);
    emit jitterBufferMsChanged();
  }
}

void CallManager::dismissCallUi() { destroyCallUi(); }

void CallManager::setCurrentCallIdentity(const QString& contactServer,
                                         const QString& contactPubKey,
                                         quint64 callId) {
  m_currentContactServer = contactServer;
  m_currentContactPubKey = contactPubKey;
  m_currentCallId = callId;
}

QString CallManager::activeCallServerId() const {
  if (!m_currentContactServer.isEmpty()) {
    return m_currentContactServer;
  }

  if (m_callUi) {
    const QVariant serverProperty = m_callUi->property("contactServer");
    if (serverProperty.isValid()) {
      return serverProperty.toString();
    }
  }

  return QString();
}

void CallManager::resetCallState(bool closeUi) {
  if (auto* androidSystemUi = AndroidSystemUi::instance()) {
    androidSystemUi->setCallProximityActive(false);
    // Only dismiss the OS incoming-call notification if THIS call is the one
    // that owns it (still un-dismissed since presentCallRequest). Otherwise a
    // call that never had one (already answered, or purely in-app) would
    // blindly cancel whatever different, still-ringing call's notification
    // happens to be live at the moment this one ends.
    if (m_incomingNotificationPending) {
      androidSystemUi->cancelIncomingCallNotification();
    }
  }
  m_incomingNotificationPending = false;

  // The call was accepted and is now ending (however it ends: local hangup,
  // remote hangup, or connection loss): stamp the "Accepted Call" log entry
  // with how long it lasted. Guarded by m_activeCallStartEpoch so this only
  // fires once per call and only for calls that actually connected.
  if (m_activeCallStartEpoch != 0) {
    const quint64 nowEpoch = static_cast<quint64>(
        QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
    const quint64 durationSec =
        nowEpoch > m_activeCallStartEpoch ? nowEpoch - m_activeCallStartEpoch : 0;
    if (m_messageModel != nullptr) {
      m_messageModel->updateCallDuration(m_currentContactServer,
                                         m_currentContactPubKey,
                                         m_currentCallId, durationSec);
    }
    if (m_db != nullptr) {
      m_db->updateCallDuration(m_currentContactServer, m_currentContactPubKey,
                               m_currentCallId, durationSec);
    }
    m_activeCallStartEpoch = 0;
  }

  secureClearCallKeys(m_sessionKeys);
  m_currentContactServer.clear();
  m_currentContactPubKey.clear();
  m_currentCallId = 0;
  m_audioSendCounter = 0;
  m_audioRecvCounter = 0;

  if (closeUi) {
    destroyCallUi();
  }
}

void CallManager::onConnectionAvailabilityChanged(const QString& serverId,
                                                  bool available) {
  if (available) {
    return;
  }

  const QString callServerId = activeCallServerId();
  if (callServerId.isEmpty() || callServerId != serverId) {
    return;
  }

  qWarning()
      << "[CallManager] Active call closed because connection was lost for"
      << serverId;
  stopAudioStream();
  resetCallState(true);
}

void CallManager::onCallDiscardReceived(const QString& serverId,
                                        const QString& contactPubKey,
                                        uint64_t callId,
                                        const QString& reason) {
  if (m_currentContactServer != serverId ||
      m_currentContactPubKey != contactPubKey || m_currentCallId != callId) {
    return;
  }

  stopAudioStream();

  // The callee was already on another call and auto-declined us as busy. Keep
  // the outgoing call window up for a moment so the QML can show "Contact is
  // busy" (it does not call discard() in this case), then tear down. The
  // teardown is guarded by callId so it never closes a different call the user
  // may have started in the meantime.
  if (reason == QStringLiteral("busy")) {
    addCallLogMessage(m_currentContactServer, m_currentContactPubKey,
                      m_currentCallId, QStringLiteral("Contact is busy"), true);
    const quint64 busyCallId = m_currentCallId;
    QTimer::singleShot(2000, this, [this, busyCallId]() {
      if (m_currentCallId == busyCallId) {
        resetCallState(true);
      }
    });
    return;
  }

  resetCallState(true);
}

void CallManager::onCallCancelReceived(const QString& serverId,
                                       const QString& contactPubKey,
                                       uint64_t callId) {
  // The caller cancelled while the incoming call was still buffered during the
  // offline sync (it was never shown). Drop it from the buffer and record a
  // missed call.
  for (int i = 0; i < m_bufferedIncomingCalls.size(); ++i) {
    const PendingIncomingCall& buffered = m_bufferedIncomingCalls.at(i);
    if (buffered.callId == callId && buffered.contactServer == serverId &&
        buffered.contactPubKey == contactPubKey) {
      m_bufferedIncomingCalls.removeAt(i);
      if (auto* androidSystemUi = AndroidSystemUi::instance()) {
        androidSystemUi->cancelIncomingCallNotification();
        androidSystemUi->dismissCallScreenIfLaunchedForCall();
      }
      addCallLogMessage(serverId, contactPubKey, callId,
                        QStringLiteral("Missed Call"), false);
      return;
    }
  }

  // The incoming call is already on screen (ringing). Close it and record a
  // missed call, unless it was already answered.
  if (m_currentContactServer == serverId &&
      m_currentContactPubKey == contactPubKey && m_currentCallId == callId) {
    const bool wasActive =
        m_callUi &&
        m_callUi->property("status").toString() == QStringLiteral("active");
    stopAudioStream();
    resetCallState(true);
    if (!wasActive) {
      addCallLogMessage(serverId, contactPubKey, callId,
                        QStringLiteral("Missed Call"), false);
      // Unanswered, FCM-launched call: return the device to its locked state.
      if (auto* androidSystemUi = AndroidSystemUi::instance()) {
        androidSystemUi->dismissCallScreenIfLaunchedForCall();
      }
    }
    return;
  }

  // Otherwise the cancel does not match any known call; ignore it.
}

void CallManager::onOfflineSyncStarted(const QString& serverId) {
  m_syncingServers.insert(serverId);

  // Drop any calls left buffered from a previous, aborted sync of this server
  // (e.g. a reconnect happened before SyncEnd). They are stale by now and the
  // fresh sync will redeliver anything that is still pending on the server.
  m_bufferedIncomingCalls.removeIf([&serverId](const PendingIncomingCall& call) {
    return call.contactServer == serverId;
  });
}

void CallManager::onOfflineSyncEnded(const QString& serverId) {
  m_syncingServers.remove(serverId);

  // Show the incoming calls that were buffered during the sync and not
  // cancelled. If several survived (rare), the last one wins, since each new
  // call UI replaces the previous one.
  QList<PendingIncomingCall> toPresent;
  for (int i = 0; i < m_bufferedIncomingCalls.size();) {
    if (m_bufferedIncomingCalls.at(i).contactServer == serverId) {
      toPresent.append(m_bufferedIncomingCalls.takeAt(i));
    } else {
      ++i;
    }
  }

  for (const PendingIncomingCall& call : toPresent) {
    presentCallRequest(call);
  }
}

void CallManager::refreshAudioOutputRoute() {
  if (!m_audioActive || !m_player) {
    return;
  }

  m_player->stop();
  m_player->start();
}

void CallManager::onAudioFrameEncoded(const QByteArray& opusData) {
  if (!m_audioActive) return;
  sendAudioFrame(opusData);
}

void CallManager::sendAudioFrame(const QByteArray& opusData) {
  if (!m_audioActive || m_sessionKeys.send_key.empty()) return;

  QByteArray encryptedFrame = encryptAudioFrame(opusData);
  m_connectionManager->sendAudioFrame(m_currentContactServer,
                                      m_currentContactPubKey, encryptedFrame);
}

void CallManager::onAudioFrameReceived(const QByteArray& encryptedFrame) {
  if (!m_audioActive) return;

  ++m_audioRecvCounter;

  QByteArray decrypted = decryptAudioFrame(encryptedFrame);

  if (decrypted.isEmpty()) {
    return;
  }

  m_player->playFrame(decrypted);
}

QByteArray CallManager::encryptAudioFrame(const QByteArray& plainFrame) {
  std::vector<uint8_t> nonce(CHACHAPOLY1305_NONCE_SIZE, 0);

  for (int i = 0; i < AUDIO_FRAME_NONCE_SIZE; ++i) {
    nonce[i] = static_cast<uint8_t>(
        (m_audioSendCounter >> (i * AUDIO_FRAME_NONCE_SIZE)) & 0xFF);
  }
  m_audioSendCounter++;

  std::vector<uint8_t> ciphertext(plainFrame.size() +
                                  crypto_aead_chacha20poly1305_ietf_ABYTES);
  unsigned long long ciphertext_len;

  crypto_aead_chacha20poly1305_ietf_encrypt(
      ciphertext.data(), &ciphertext_len,
      reinterpret_cast<const unsigned char*>(plainFrame.constData()),
      plainFrame.size(), nullptr, 0, nullptr, nonce.data(),
      m_sessionKeys.send_key.data());

  QByteArray result;
  result.append(reinterpret_cast<const char*>(nonce.data()),
                AUDIO_FRAME_NONCE_SIZE);
  result.append(reinterpret_cast<const char*>(ciphertext.data()),
                static_cast<int>(ciphertext_len));

  return result;
}

QByteArray CallManager::decryptAudioFrame(const QByteArray& encryptedFrame) {
  if (encryptedFrame.size() <
      AUDIO_FRAME_NONCE_SIZE + crypto_aead_chacha20poly1305_ietf_ABYTES) {
    // qWarning() << "[CallManager] Audio frame too short";
    return {};
  }

  std::vector<uint8_t> nonce(CHACHAPOLY1305_NONCE_SIZE, 0);
  memcpy(nonce.data(), encryptedFrame.constData(), AUDIO_FRAME_NONCE_SIZE);

  const unsigned char* ciphertext = reinterpret_cast<const unsigned char*>(
      encryptedFrame.constData() + AUDIO_FRAME_NONCE_SIZE);
  size_t ciphertext_len = encryptedFrame.size() - AUDIO_FRAME_NONCE_SIZE;

  std::vector<uint8_t> plaintext(ciphertext_len -
                                 crypto_aead_chacha20poly1305_ietf_ABYTES);
  unsigned long long plaintext_len;

  try {
    int result = crypto_aead_chacha20poly1305_ietf_decrypt(
        plaintext.data(), &plaintext_len, nullptr, ciphertext, ciphertext_len,
        nullptr, 0, nonce.data(), m_sessionKeys.receive_key.data());

    if (result != 0) {
      qWarning() << "[CallManager] Audio frame decryption failed";
      return {};
    }
  } catch (...) {
    qWarning() << "[CallManager] Exception during audio frame decryption";
    return {};
  }

  return QByteArray(reinterpret_cast<const char*>(plaintext.data()),
                    static_cast<int>(plaintext_len));
}

void CallManager::onRecorderError(const QString& error) {
  qWarning() << "[CallManager] Recorder error:" << error;
  emit audioError(error);
}

void CallManager::onPlayerError(const QString& error) {
  qWarning() << "[CallManager] Player error:" << error;
  emit audioError(error);
}

void CallManager::onRemoteVoiceLevelChanged(qreal level) {
  if (qFuzzyCompare(m_remoteVoiceLevel, level)) return;

  m_remoteVoiceLevel = level;

  emit remoteVoiceLevelChanged(level);
}