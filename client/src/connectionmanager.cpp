#include "connectionmanager.h"

#include <corecrypto.h>

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QUuid>
#include <QWindow>
#include <QtConcurrent>

#include "connectionpayloadprocessor.h"
#include "coreutils.h"
#include "doubleratchet.h"
#include "filemessageutils.h"
#include "inviteutils.h"
#include "pq.h"
#include "pqdh.h"
#include "prekeys.h"
#include "settingsmanager.h"

ConnectionManager::ConnectionManager(QObject* parent,
                                     ConnectionStorage* storage,
                                     AccountManager* accountManager,
                                     ContactsModel* contactsModel,
                                     SettingsManager* settingsManager,
                                     MessageModel* messageModel)
    : QObject(parent),
      m_storage(storage),
      m_accountManager(accountManager),
      lastRequestId_(0),
      m_contactsModel(contactsModel),
      m_settings(settingsManager),
      m_messageModel(messageModel) {
  m_transportThread = new QThread(this);
  m_transport = new ConnectionTransport();
  m_transport->moveToThread(m_transportThread);
  connect(m_transportThread, &QThread::finished, m_transport,
          &QObject::deleteLater);
  m_transportThread->start();

  connect(m_transport, &ConnectionTransport::textMessageReceived, this,
          [this](const QString& serverId, const QString& message) {
            emit messageReceived(serverId, message);
          });
  connect(m_transport, &ConnectionTransport::binaryMessageReceived, this,
          &ConnectionManager::onTransportBinaryMessageReceived);
  connect(m_transport, &ConnectionTransport::disconnected, this,
          &ConnectionManager::onTransportDisconnected);
  connect(m_transport, &ConnectionTransport::socketErrorOccurred, this,
          &ConnectionManager::onTransportError);
  connect(m_transport, &ConnectionTransport::sslErrorOccurred, this,
          &ConnectionManager::onTransportSslError);
  connect(m_transport, &ConnectionTransport::pongReceived, this,
          [this](const QString& serverId, quint64 elapsedTime) {
            emit pongReceived(serverId, elapsedTime);
          });
  connect(m_transport, &ConnectionTransport::connectionStatusChanged, this,
          [this](const QString& serverId, int status) {
            emit connectionStatusChanged(serverId,
                                         static_cast<ConnectionStatus>(status));
          });
  connect(this, &ConnectionManager::authChanged, this,
          [this](const QString& serverId, bool success) {
            if (success) {
              updatePeriodicPrekeySyncTimer();
              retryPendingTextMessages(serverId);
              retryPendingMemberEnrollments(serverId);
              requestPrekeysSync(serverId);
            }
          });
  connect(&m_periodicPrekeySyncTimer, &QTimer::timeout, this,
          &ConnectionManager::onPeriodicPrekeySyncTimeout);
  connect(m_settings, &SettingsManager::settingChanged, this,
          [this](const QString& settingName, const QString&) {
            if (settingName == QStringLiteral("prekeysSyncIntervalMinutes")) {
              updatePeriodicPrekeySyncTimer();
            }
          });
  updatePeriodicPrekeySyncTimer();

  connect(&MainSignals::instance(), &MainSignals::onlineRequest, this,
          &ConnectionManager::onOnlineRequest);
  connect(&MainSignals::instance(), &MainSignals::messageDelete, this,
          &ConnectionManager::onMessageDelete);
  connect(&MainSignals::instance(), &MainSignals::contactRemove, this,
          &ConnectionManager::onContactRemove);
  connect(&MainSignals::instance(), &MainSignals::contactRemovedLocally, this,
          [this](const QString& serverId, const QString&) {
            cleanupServerIfUnused(serverId);
          });
  connect(&MainSignals::instance(), &MainSignals::clearHistory, this,
          &ConnectionManager::onClearHistory);
  connect(&MainSignals::instance(), &MainSignals::setTimer, this,
          &ConnectionManager::onSetTimer);

#ifdef Q_OS_ANDROID
  // On mobile, go offline while backgrounded so incoming calls/messages are
  // delivered via FCM (a backgrounded app can't surface a call and the server
  // would keep us "online"); reconnect + resync on foreground.
  connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
          &ConnectionManager::onApplicationStateChanged);
#endif

  ConnectionPayloadProcessor::Callbacks payloadCallbacks;
  payloadCallbacks.sendReceivedSignalForMessage =
      [this](const QString& serverId, const QString& fromPubKey,
             quint64 messageId) {
        sendReceivedSignalForMessage(serverId, fromPubKey, messageId);
      };
  // Only used to reply with our own accountInfo (a profile sync), so tag it
  // Silent — an offline peer stores it without a spurious "new message" push.
  payloadCallbacks.sendJson = [this](const QString& serverId,
                                     const QString& toPubKey,
                                     const QString& jsonStr) {
    return e2eSendJsonWithPush(serverId, toPubKey, jsonStr, PushKind::Silent);
  };
  payloadCallbacks.buildOwnAccountInfoJson = [this](bool isResponse) {
    return buildOwnAccountInfoJson(isResponse);
  };
  payloadCallbacks.setCallSessionState =
      [this](const CoreCrypto::SessionKeys& sessionKeys,
             const QString& serverId, const QString& contactPubKey) {
        setCallSessionState(sessionKeys, serverId, contactPubKey);
      };
  payloadCallbacks.textMessageReceived =
      [this](const QString& serverId, const QString& fromPubKey,
             const QString& message, quint64 mid) {
        emit textMessageReceived(serverId, fromPubKey, message, mid);
      };
  payloadCallbacks.contactAvatarChanged = [this](const QString& serverId,
                                                 const QString& contactPubKey) {
    emit contactAvatarChanged(serverId, contactPubKey);
  };
  payloadCallbacks.callDiscardReceived = [this](const QString& serverId,
                                                const QString& contactPubKey,
                                                quint64 callId,
                                                const QString& reason) {
    emit callDiscardReceived(serverId, contactPubKey, callId, reason);
  };
  payloadCallbacks.callCancelReceived = [this](const QString& serverId,
                                               const QString& contactPubKey,
                                               quint64 callId) {
    emit callCancelReceived(serverId, contactPubKey, callId);
  };
  payloadCallbacks.callAcceptReceived = [this]() { emit callAcceptReceived(); };
  payloadCallbacks.callRequestReceived =
      [this](const QString& contactFirstName, const QString& contactAvatar,
             const QString& contactServer, const QString& contactPubKey,
             const QString& dhPub, const QString& id, uint64_t callId) {
        emit callRequestReceived(contactFirstName, contactAvatar, contactServer,
                                 contactPubKey, dhPub, id, callId);
      };
  // Our current invite secret seeds the initial ratchet root key for incoming
  // session_requests (the receiver side of the invite gate).
  payloadCallbacks.getOwnInviteSecret = [this]() {
    return m_accountManager->getOrCreateInviteSecret();
  };
  // When a peer that scanned our QR establishes a session, admit them to the
  // server whitelist (server enforces that we must be a member ourselves).
  payloadCallbacks.enrollMember = [this](const QString& serverId,
                                         const QString& contactPubKey) {
    enrollMember(serverId, contactPubKey);
  };

  ConnectionProtocolHandler::Callbacks protocolCallbacks;
  protocolCallbacks.authChanged = [this](const QString& serverId,
                                         bool success) {
    emit authChanged(serverId, success);
  };
  protocolCallbacks.protocolVersionMismatch = [this](const QString& serverId,
                                                     const QString& message) {
    m_transport->setReconnectBlocked(serverId, true);
    emit protocolVersionMismatch(serverId, message);
  };
  protocolCallbacks.invokeCallback =
      [this](uint32_t requestId, const QJSValueList& args,
             const QString& context, const QString& serverId,
             const QString& state) {
        invokeCallback(requestId, args, context, serverId, state);
      };
  protocolCallbacks.fcmTokenRegistered = [this](const QString& serverId,
                                                uint32_t requestId) {
    emit fcmTokenRegistered(serverId, requestId);
  };
  protocolCallbacks.fcmNotUsed = [this](const QString& serverId,
                                        uint32_t requestId) {
    emit fcmNotUsed(serverId, requestId);
  };
  protocolCallbacks.enrollAcked = [this](const QString& serverId,
                                        uint32_t requestId) {
    handleEnrollAcked(serverId, requestId);
  };
  protocolCallbacks.sendRequest = [this](const QString& serverId,
                                         uint8_t requestType,
                                         const QByteArray& payload) {
    return request(serverId,
                   static_cast<ConnectionManager::RequestType>(requestType),
                   payload);
  };
  protocolCallbacks.prekeyTargetCount = [this]() {
    return prekeyTargetCount();
  };
  protocolCallbacks.prekeyGenerationStateChanged =
      [this](const QString& serverId, bool active, int generatedCount,
             int targetCount) {
        if (active) {
          m_prekeyGenerationActiveServers.insert(serverId);
          m_prekeyGenerationStatusTexts.insert(serverId,
                                               tr("Generating prekeys"));
        } else {
          m_prekeyGenerationActiveServers.remove(serverId);
          m_prekeyGenerationStatusTexts.remove(serverId);
        }
        emit prekeyGenerationStateChanged(serverId, active, generatedCount,
                                          targetCount);
      };
  protocolCallbacks.handleForwardFrame = [this](const QString& serverId,
                                                const QByteArray& data) {
    m_payloadProcessor->handleForwardFrame(serverId, data);
  };
  protocolCallbacks.sendMessageAck = [this](const QString& serverId,
                                            quint64 dbId) {
    QByteArray payload;
    payload.push_back(static_cast<char>(static_cast<uint8_t>(RequestType::MESSAGE_ACK)));
    for (int i = 7; i >= 0; --i) {
      payload.push_back(static_cast<char>((dbId >> (i * 8)) & 0xFF));
    }
    sendBinaryMessage(serverId, payload);
  };
  protocolCallbacks.audioFrameReceived = [this](const QByteArray& frame) {
    emit audioFrameReceived(frame);
  };
  protocolCallbacks.authenticated = [this](const QString& serverId) {
    emit authenticated(serverId);
  };
  protocolCallbacks.syncStarted = [this](const QString& serverId) {
    // Widen the transport's dead-connection watchdog for the duration of the
    // sync: the backlog stream keeps the link busy but a pong can lag behind it
    // in the TCP byte stream, and the tight 20s window would tear the socket
    // down mid-sync and reconnect-loop forever.
    m_transport->setSyncInProgress(serverId, true);
    MainSignals::instance().emitSyncStarted(serverId);
    emit offlineSyncStarted(serverId);
  };
  protocolCallbacks.syncEnded = [this](const QString& serverId) {
    m_transport->setSyncInProgress(serverId, false);
    MainSignals::instance().emitSyncEnded(serverId);
    emit offlineSyncEnded(serverId);
  };

  m_protocolHandler = std::make_unique<ConnectionProtocolHandler>(
      m_accountManager, m_contactsModel, m_messageModel,
      std::move(protocolCallbacks));

  m_payloadProcessor = std::make_unique<ConnectionPayloadProcessor>(
      m_storage, m_accountManager, m_contactsModel, m_messageModel,
      std::move(payloadCallbacks));

  qDebug() << "[ConnectionManager] Initialized successfully";
}

ConnectionManager::~ConnectionManager() {
  removeAllServers();
  m_transportThread->quit();
  m_transportThread->wait();
  qDebug() << "[ConnectionManager] Destroyed";
}

ConnectionServerRuntimeState& ConnectionManager::stateForServer(
    const QString& serverId) {
  return m_serverState[serverId];
}

const ConnectionServerRuntimeState* ConnectionManager::stateForServer(
    const QString& serverId) const {
  auto it = m_serverState.constFind(serverId);
  if (it == m_serverState.constEnd()) {
    return nullptr;
  }
  return &it.value();
}

void ConnectionManager::addServer(const QString& serverUrl,
                                  const QString& serverId) {
  QString id = serverId.isEmpty() ? generateServerId(serverUrl) : serverId;
  bool shouldReconnect = false;
  bool serverCreated = false;

  if (m_transport->hasServer(id)) {
    const ConnectionTransport::ConnectionStatus currentStatus =
        m_transport->status(id);
    qWarning() << "[ConnectionManager] Server" << serverUrl
               << "already exists:" << id;
    shouldReconnect =
        currentStatus != ConnectionTransport::ConnectionStatus::Connected &&
        currentStatus != ConnectionTransport::ConnectionStatus::Connecting &&
        currentStatus != ConnectionTransport::ConnectionStatus::Reconnecting;
  } else {
    qWarning() << "[ConnectionManager] Adding server:" << id << serverUrl;
    serverCreated = m_transport->addServer(id, serverUrl);
    if (serverCreated) {
      m_serverState.insert(id, ConnectionServerRuntimeState{});
      emit authChanged(id, false);
    }
  }

  if (shouldReconnect) {
    qDebug() << "[ConnectionManager] Reconnecting existing server:" << id;
    m_transport->connectServer(id);
  }

  if (serverCreated) {
    emit serverAdded(id);
  }
}

void ConnectionManager::requestPrekeysSync(const QString& serverId) {
  if (!m_transport->hasServer(serverId)) {
    qDebug() << "[ConnectionManager] [requestPrekeysSync] Server not found:"
             << serverId;
    return;
  }

  if (m_transport->status(serverId) !=
      ConnectionTransport::ConnectionStatus::Connected) {
    qDebug()
        << "[ConnectionManager] [requestPrekeysSync] Server is not connected:"
        << serverId;
    return;
  }

  ConnectionServerRuntimeState& runtimeState = stateForServer(serverId);

  if (!runtimeState.authCompleted && !runtimeState.authResponseSent) {
    qDebug() << "[ConnectionManager] [requestPrekeysSync] Auth handshake has "
                "not started yet for"
             << serverId;
    return;
  }

  if (runtimeState.prekeysSyncInFlight) {
    qDebug()
        << "[ConnectionManager] [requestPrekeysSync] Sync already in flight for"
        << serverId;
    return;
  }

  if (m_prekeyGenerationActiveServers.contains(serverId)) {
    qDebug() << "[ConnectionManager] [requestPrekeysSync] Prekey generation "
                "already active for"
             << serverId;
    return;
  }

  runtimeState.prekeysSyncInFlight = true;

  qDebug()
      << "[ConnectionManager] [requestPrekeysSync] Requesting prekeys count for"
      << serverId;
  const uint32_t requestId =
      request(serverId, ConnectionManager::RequestType::COUNT_PREKEYS);
  if (requestId == 0) {
    stateForServer(serverId).prekeysSyncInFlight = false;
    qWarning() << "[ConnectionManager] [requestPrekeysSync] Failed to request "
                  "prekeys count for"
               << serverId;
  }
}

void ConnectionManager::removeServer(const QString& serverId) {
  if (serverId.isEmpty() || !m_transport->hasServer(serverId)) {
    return;
  }

  qDebug() << "[ConnectionManager] Removing server:" << serverId;

  clearPendingRetryRequests(serverId);
  m_prekeyGenerationActiveServers.remove(serverId);
  m_prekeyGenerationStatusTexts.remove(serverId);
  m_serverState.remove(serverId);

  if (m_currentContactServer == serverId) {
    m_currentContactServer.clear();
    m_currentContactPubKey.clear();
    m_callKeys = CoreCrypto::SessionKeys{};
  }

  m_transport->removeServer(serverId);
  emit authChanged(serverId, false);
  emit serverRemoved(serverId);
}

void ConnectionManager::removeAllServers() {
  qDebug() << "[ConnectionManager] Removing all servers";
  m_transport->removeAllServers();
  m_serverState.clear();
}

bool ConnectionManager::sendMessage(const QString& serverId,
                                    const QString& message) {
  return m_transport->sendTextMessage(serverId, message);
}

bool ConnectionManager::sendBinaryMessage(const QString& serverId,
                                          const QByteArray& data) {
  return m_transport->sendBinaryMessage(serverId, data);
}

uint32_t ConnectionManager::request(const QString& serverId,
                                    ConnectionManager::RequestType requestType,
                                    const QByteArray& payload) {
  if (!m_transport->hasServer(serverId)) {
    qWarning() << "[ConnectionManager] Server not found:" << serverId;
    return 0;
  }

  if (!m_transport->isConnected(serverId)) {
    qWarning() << "[ConnectionManager] Server not connected:" << serverId;
    return 0;
  }

  uint32_t requestId = ++lastRequestId_;

  QByteArray requestData;

  requestData.append(static_cast<uint8_t>(requestType));

  requestData.append(static_cast<uint8_t>((requestId >> 24) & 0xFF));
  requestData.append(static_cast<uint8_t>((requestId >> 16) & 0xFF));
  requestData.append(static_cast<uint8_t>((requestId >> 8) & 0xFF));
  requestData.append(static_cast<uint8_t>(requestId & 0xFF));

  requestData.append(payload);

  if (m_transport->sendBinaryMessage(serverId, requestData)) {
    return requestId;
  }

  qWarning() << "[ConnectionManager] Failed to send request to" << serverId;
  return 0;
}

bool ConnectionManager::isConnected(const QString& serverId) const {
  return m_transport->isConnected(serverId);
}

bool ConnectionManager::isAuthenticated(const QString& serverId) const {
  const ConnectionServerRuntimeState* runtimeState = stateForServer(serverId);
  const bool authenticated =
      runtimeState != nullptr && runtimeState->authCompleted;
  qDebug() << "[ConnectionManager] Authentication status for server" << serverId
           << "is" << (authenticated ? "Authenticated" : "Not Authenticated");
  return authenticated;
}

int ConnectionManager::connectionPhase(const QString& serverId) const {
  if (!m_transport->isConnected(serverId)) {
    return 0;  // not connected (the transport/port steps show the details)
  }
  const ConnectionServerRuntimeState* runtimeState = stateForServer(serverId);
  if (runtimeState == nullptr || !runtimeState->authenticated) {
    return 2;  // connected, handshake not yet confirmed -> "authenticating"
  }
  if (!runtimeState->authCompleted) {
    return 3;  // authenticated, backlog draining -> "synchronizing"
  }
  return 4;  // AUTH_SUCCESS seen -> fully ready
}

QString ConnectionManager::getAccessToken(const QString& serverId) const {
  if (!m_transport->hasServer(serverId)) {
    qWarning() << "[ConnectionManager] [getAccessToken] Server not found for "
                  "token lookup:"
               << serverId << "knownServers=" << m_transport->allServers();
    return QString();
  }

  const ConnectionServerRuntimeState* runtimeState = stateForServer(serverId);
  const QString accessToken =
      runtimeState != nullptr ? runtimeState->accessToken : QString();
  qDebug() << "[ConnectionManager] [getAccessToken] Lookup for server"
           << serverId << "authCompleted="
           << (runtimeState != nullptr && runtimeState->authCompleted)
           << "tokenEmpty=" << accessToken.isEmpty()
           << "tokenLength=" << accessToken.size();

  return accessToken;
}

QString ConnectionManager::getHttpsBaseUrl(const QString& serverId) const {
  return m_transport->httpsBaseUrl(serverId);
}

ConnectionManager::ConnectionStatus ConnectionManager::getStatus(
    const QString& serverId) const {
  return static_cast<ConnectionStatus>(m_transport->status(serverId));
}

QStringList ConnectionManager::getConnectedServers() const {
  return m_transport->connectedServers();
}

QStringList ConnectionManager::getAllServers() const {
  return m_transport->allServers();
}

void ConnectionManager::setReconnectInterval(int milliseconds) {
  m_transport->setReconnectInterval(milliseconds);
  qDebug() << "[ConnectionManager] Reconnect interval set to" << milliseconds
           << "ms";
}

void ConnectionManager::setMaxReconnectAttempts(int attempts) {
  m_transport->setMaxReconnectAttempts(attempts);
  qDebug() << "[ConnectionManager] Max reconnect attempts set to" << attempts;
}

void ConnectionManager::setConnectionTimeout(int milliseconds) {
  m_transport->setConnectionTimeout(milliseconds);
  qDebug() << "[ConnectionManager] Connection timeout set to" << milliseconds
           << "ms";
}

void ConnectionManager::setPingInterval(int milliseconds) {
  m_transport->setPingInterval(milliseconds);
  qDebug() << "[ConnectionManager] Ping interval set to" << milliseconds
           << "ms";
}

void ConnectionManager::setDeadConnectionTimeout(int milliseconds) {
  m_transport->setDeadConnectionTimeout(milliseconds);
  qDebug() << "[ConnectionManager] Dead connection timeout set to"
           << milliseconds << "ms";
}

bool ConnectionManager::isPrekeyGenerationActive(
    const QString& serverId) const {
  return m_prekeyGenerationActiveServers.contains(serverId);
}

QString ConnectionManager::prekeyGenerationStatusText(
    const QString& serverId) const {
  return m_prekeyGenerationStatusTexts.value(serverId);
}

void ConnectionManager::invokeCallback(uint32_t requestId,
                                       const QJSValueList& args,
                                       const QString& context,
                                       const QString& serverId,
                                       const QString& state) {
  QJSValue callback = callbacks_.value(requestId);
  if (callback.isCallable()) {
    qDebug() << context << "Invoking callback for request ID" << requestId
             << "from" << serverId;
    callback.call(args);
  } else {
    qWarning() << context << "No valid callback found for request ID"
               << requestId << "from" << serverId;
  }

  if (!state.isEmpty()) {
    handlePendingRetryRequestState(requestId, state);
    emit requestStateChanged(requestId, state);
  }
}

QString ConnectionManager::pendingRetryMessageKey(const QString& serverId,
                                                  const QString& contactPubKey,
                                                  quint64 messageId) const {
  return serverId + QStringLiteral("|") + contactPubKey + QStringLiteral("|") +
         QString::number(messageId);
}

void ConnectionManager::retryPendingTextMessages(const QString& serverId) {
  if (m_storage == nullptr || !isConnected(serverId) ||
      !isAuthenticated(serverId)) {
    return;
  }

  const QList<PendingTextMessage> pendingMessages =
      m_storage->loadPendingTextMessages(serverId);
  for (const PendingTextMessage& pendingMessage : pendingMessages) {
    const QString key = pendingRetryMessageKey(
        serverId, pendingMessage.contactPubKey, pendingMessage.id);
    if (m_pendingRetryMessageKeys.contains(key)) {
      continue;
    }

    const quint64 resendTimestamp = static_cast<quint64>(
        QDateTime::currentDateTimeUtc().toSecsSinceEpoch());

    nlohmann::json messageJson;
    messageJson["type"] = "text";
    messageJson["timestamp"] = static_cast<uint64_t>(resendTimestamp);
    messageJson["text"] = pendingMessage.text.toStdString();
    messageJson["message_id"] = static_cast<uint64_t>(pendingMessage.id);
    // Carry the disappearing-message TTL so the peer stores the same value our
    // local copy already has, keeping both sides' timer sweeps in sync (matches
    // the "timer" field the normal send path stamps in Main.qml).
    int retryTimerSeconds = 0;
    if (m_contactsModel != nullptr) {
      const auto contact =
          m_contactsModel->getContact(serverId, pendingMessage.contactPubKey);
      if (contact.has_value()) {
        retryTimerSeconds = contact->timerValue;
      }
    }
    messageJson["timer"] = retryTimerSeconds;

    const uint32_t requestId =
        e2eSendJSON(serverId, pendingMessage.contactPubKey,
                    QString::fromStdString(messageJson.dump()));
    if (requestId == 0) {
      continue;
    }

    m_pendingRetryMessageKeys.insert(key);
    m_pendingRetryRequests.insert(
        requestId, PendingRetryRequest{serverId, pendingMessage.contactPubKey,
                                       pendingMessage.id, pendingMessage.text,
                                       resendTimestamp});

    if (m_messageModel != nullptr) {
      m_messageModel->updateMessageTimestamp(
          serverId, pendingMessage.contactPubKey, pendingMessage.id,
          resendTimestamp);
    }
    if (m_contactsModel != nullptr) {
      m_contactsModel->setLastMessageAt(serverId, pendingMessage.contactPubKey,
                                        pendingMessage.text, resendTimestamp);
    }
    m_storage->updateMessageTimestamp(serverId, pendingMessage.contactPubKey,
                                      pendingMessage.id, resendTimestamp);
  }
}

void ConnectionManager::handlePendingRetryRequestState(uint32_t requestId,
                                                       const QString& state) {
  auto it = m_pendingRetryRequests.find(requestId);
  if (it == m_pendingRetryRequests.end()) {
    return;
  }

  const PendingRetryRequest pending = it.value();
  const QString key = pendingRetryMessageKey(
      pending.serverId, pending.contactPubKey, pending.messageId);

  if (state == QStringLiteral("received")) {
    if (m_messageModel != nullptr) {
      m_messageModel->updateMessageSentStatus(
          pending.serverId, pending.contactPubKey, pending.messageId);
    }
    if (m_storage != nullptr) {
      m_storage->updateMessageSentStatus(
          pending.serverId, pending.contactPubKey, pending.messageId);
    }
    m_pendingRetryRequests.erase(it);
    m_pendingRetryMessageKeys.remove(key);
    return;
  }

  if (state == QStringLiteral("delivered")) {
    if (m_messageModel != nullptr) {
      m_messageModel->updateMessageSentStatus(
          pending.serverId, pending.contactPubKey, pending.messageId);
      m_messageModel->updateMessageDeliveredStatus(
          pending.serverId, pending.contactPubKey, pending.messageId);
    }
    if (m_storage != nullptr) {
      m_storage->updateMessageSentStatus(
          pending.serverId, pending.contactPubKey, pending.messageId);
      m_storage->updateMessageDeliveredStatus(
          pending.serverId, pending.contactPubKey, pending.messageId);
    }
    m_pendingRetryRequests.erase(it);
    m_pendingRetryMessageKeys.remove(key);
  }
}

void ConnectionManager::clearPendingRetryRequests(const QString& serverId) {
  for (auto it = m_pendingRetryRequests.begin();
       it != m_pendingRetryRequests.end();) {
    if (it.value().serverId != serverId) {
      ++it;
      continue;
    }

    m_pendingRetryMessageKeys.remove(pendingRetryMessageKey(
        it.value().serverId, it.value().contactPubKey, it.value().messageId));
    it = m_pendingRetryRequests.erase(it);
  }
}

bool ConnectionManager::isHomeServer(const QString& serverId) const {
  return m_settings != nullptr &&
         m_settings->getTextSetting(QStringLiteral("serverAddress"))
                 .trimmed() == serverId;
}

bool ConnectionManager::serverHasContacts(const QString& serverId) const {
  if (m_contactsModel == nullptr) {
    return false;
  }

  const QList<QPair<QString, QString>> endpoints =
      m_contactsModel->contactEndpoints();
  for (const auto& endpoint : endpoints) {
    if (endpoint.first == serverId) {
      return true;
    }
  }

  return false;
}

void ConnectionManager::cleanupServerIfUnused(const QString& serverId) {
  if (serverId.isEmpty() || !m_transport->hasServer(serverId)) {
    return;
  }

  if (isHomeServer(serverId)) {
    qDebug() << "[ConnectionManager] Keeping home server after contact removal:"
             << serverId;
    return;
  }

  if (serverHasContacts(serverId)) {
    qDebug() << "[ConnectionManager] Keeping server with remaining contacts:"
             << serverId;
    return;
  }

  removeServer(serverId);
}

int ConnectionManager::prekeyTargetCount() const {
  if (m_settings == nullptr) {
    return PREKEYS_MAX_SIZE;
  }

  bool ok = false;
  const int value = m_settings
                        ->getTextSetting(QStringLiteral("prekeysTargetCount"),
                                         QString::number(PREKEYS_MAX_SIZE))
                        .toInt(&ok);
  if (!ok) {
    return PREKEYS_MAX_SIZE;
  }

  return qBound(1, value, 1000);
}

int ConnectionManager::prekeySyncIntervalMinutes() const {
  if (m_settings == nullptr) {
    return 5;
  }

  bool ok = false;
  const int value =
      m_settings
          ->getTextSetting(QStringLiteral("prekeysSyncIntervalMinutes"),
                           QStringLiteral("5"))
          .toInt(&ok);
  if (!ok) {
    return 5;
  }

  return qBound(1, value, 1440);
}

void ConnectionManager::updatePeriodicPrekeySyncTimer() {
  const int intervalMs = prekeySyncIntervalMinutes() * 60 * 1000;
  m_periodicPrekeySyncTimer.setInterval(intervalMs);
  if (!m_periodicPrekeySyncTimer.isActive()) {
    m_periodicPrekeySyncTimer.start();
  }
}

void ConnectionManager::onPeriodicPrekeySyncTimeout() {
  const QStringList serverIds = getAllServers();
  for (const QString& serverId : serverIds) {
    if (!isConnected(serverId) || !isAuthenticated(serverId)) {
      continue;
    }

    requestPrekeysSync(serverId);
  }
}

void ConnectionManager::onTransportBinaryMessageReceived(
    const QString& serverId, const QByteArray& data) {
  try {
    QByteArray responseToSend;
    QString prekeysSyncServerId;
    ConnectionServerRuntimeState& runtimeState = stateForServer(serverId);
    m_protocolHandler->handleBinaryFrame(serverId, data, &runtimeState,
                                         &responseToSend, &prekeysSyncServerId);

    if (!serverId.isEmpty()) {
      if (!responseToSend.isEmpty()) {
        sendBinaryMessage(serverId, responseToSend);
      }
      if (!prekeysSyncServerId.isEmpty()) {
        requestPrekeysSync(prekeysSyncServerId);
      }
      emit binaryMessageReceived(serverId, data);
    }
  } catch (const std::exception& e) {
    // НЕ логировать сам payload (приватность) — только serverId и тип первого байта.
    qWarning() << "[ConnectionManager] dropped malformed frame from" << serverId
               << "type=" << (data.isEmpty() ? -1 : int(uchar(data[0])))
               << "err=" << e.what();
  } catch (...) {
    qWarning() << "[ConnectionManager] dropped malformed frame (unknown exc) from" << serverId;
  }
}

void ConnectionManager::onTransportDisconnected(const QString& serverId) {
  qWarning() << "[ConnectionManager] onDisconnected completed" << serverId;
  clearPendingRetryRequests(serverId);
  m_prekeyGenerationActiveServers.remove(serverId);
  m_prekeyGenerationStatusTexts.remove(serverId);
  if (m_transport->hasServer(serverId)) {
    m_serverState[serverId] = ConnectionServerRuntimeState{};
  } else {
    m_serverState.remove(serverId);
  }
  emit authChanged(serverId, false);
}

void ConnectionManager::onTransportError(const QString& serverId,
                                         const QString& errorMessage) {
  qWarning() << "[ConnectionManager] [onError] Socket error for" << serverId
             << ':' << errorMessage;
  clearPendingRetryRequests(serverId);
  m_prekeyGenerationActiveServers.remove(serverId);
  m_prekeyGenerationStatusTexts.remove(serverId);
  if (m_transport->hasServer(serverId)) {
    m_serverState[serverId] = ConnectionServerRuntimeState{};
  } else {
    m_serverState.remove(serverId);
  }
  emit authChanged(serverId, false);
}

void ConnectionManager::onTransportSslError(const QString& serverId,
                                            const QString& errorMessage) {
  qWarning() << "[ConnectionManager] [onSslErrors] SSL Error for" << serverId
             << ':' << errorMessage;
}

QString ConnectionManager::generateServerId(const QString& serverUrl) {
  return serverUrl;
}

void ConnectionManager::addServers() {
  qDebug() << "[ConnectionManager] [addServers] Starting to add servers from "
              "database";

  if (m_accountManager->getPublicKey().isEmpty()) {
    qWarning() << "[ConnectionManager] [addServers] No public key available";
    return;
  }

  const QStringList addresses = m_storage->loadKnownServerAddresses();

  for (const QString& address : addresses) {
    qDebug() << "[ConnectionManager] [addServers] Adding server from database:"
             << address;
    addServer(address);
  }

  qDebug() << "[ConnectionManager] [addServers] Servers added from database";
}

void ConnectionManager::goOffline() {
  if (m_suspendedOffline) {
    return;
  }
  qWarning() << "[ConnectionManager] going offline so incoming calls/messages "
                "arrive via FCM";
  m_suspendedOffline = true;
  removeAllServers();
}

void ConnectionManager::goOnline() {
  if (!m_suspendedOffline) {
    return;
  }
  qWarning() << "[ConnectionManager] going online, reconnecting and resyncing";
  m_suspendedOffline = false;
  addServers();
}

void ConnectionManager::onApplicationStateChanged(Qt::ApplicationState state) {
  if (state == Qt::ApplicationActive) {
    // Coming back from another activity on top (e.g. Android's native
    // file/photo picker) can leave the Qt Quick scene graph's rendering
    // surface stale — parts of the UI (like the compose bar) don't repaint
    // until something else forces a frame. Nudge every top-level window.
    for (QWindow* window : QGuiApplication::allWindows()) {
      if (auto* quickWindow = qobject_cast<QQuickWindow*>(window)) {
        quickWindow->update();
      }
    }

    // Back in the foreground: reconnect and replay anything (messages, call
    // signaling) the server stored while we were offline.
    m_appBackgrounded = false;
    goOnline();
    return;
  }

  // ApplicationInactive is transient (notification shade, a system dialog) — do
  // nothing. Only a real background (Suspended/Hidden) takes us offline.
  if (state != Qt::ApplicationSuspended && state != Qt::ApplicationHidden) {
    return;
  }

  m_appBackgrounded = true;
  // Never disconnect mid-call: that would drop the conversation. We stay
  // connected while a call UI is up; setCallActive(false) takes us offline once
  // the call ends if we are still backgrounded.
  if (m_callActive) {
    qDebug() << "[ConnectionManager] backgrounded during a call, staying online";
    return;
  }
  goOffline();
}

void ConnectionManager::setCallActive(bool active) {
  m_callActive = active;
  if (active) {
    return;
  }

  // The call UI was torn down. If that happened while backgrounded (a call had
  // kept us connected), go offline now. Deferred so a UI *replacement*
  // (createCallUi destroys then immediately recreates) doesn't trip this — by
  // the time it runs, m_callActive is true again.
  QMetaObject::invokeMethod(
      this,
      [this]() {
        if (!m_callActive && m_appBackgrounded) {
          goOffline();
        }
      },
      Qt::QueuedConnection);
}

bool ConnectionManager::requestPreKey(const QString& serverId,
                                      const QString& clientPubKey,
                                      QJSValue callback) {
  if (!this->isConnected(serverId)) {
    if (callback.isCallable()) callback.call(QJSValueList() << false);
    return false;
  }

  qDebug()
      << "[ConnectionManager] [requestPreKey] Requesting prekey from server"
      << serverId << "for clientPubKey" << clientPubKey;

  std::vector<uint8_t> clientPubKeyVec =
      coreutils::hex_to_bytes(clientPubKey.toStdString());
  QByteArray clientPubKeyArray(
      reinterpret_cast<const char*>(clientPubKeyVec.data()),
      static_cast<int>(clientPubKeyVec.size()));

  QByteArray prekeysRequest;
  prekeysRequest.append(clientPubKeyArray);

  uint32_t requestId = request(
      serverId, ConnectionManager::RequestType::GET_PREKEY, prekeysRequest);
  qDebug()
      << "[ConnectionManager] [requestPreKey] Storing callback for requestId"
      << requestId;
  callbacks_[requestId] = callback;

  qDebug() << "[ConnectionManager] [requestPreKey] Requested prekey from server"
           << serverId;

  return true;
}

void ConnectionManager::isOnline(const QString& serverId,
                                 const QString& contactPubKey,
                                 QJSValue callback) {
  if (!this->isConnected(serverId)) {
    if (callback.isCallable()) callback.call(QJSValueList() << false);
    return;
  }

  QByteArray contactPubKeyArr = QByteArray::fromHex(contactPubKey.toUtf8());

  uint32_t requestId =
      request(serverId, ConnectionManager::RequestType::GET_ONLINE_STATUS,
              contactPubKeyArr);
  callbacks_[requestId] = callback;
}

bool ConnectionManager::startSessionWithPreKey(
    const QString& serverId, const QString& prekeyId, const QString& dh_pub,
    const QString& pq_pub, const QString& ed_pub, const QString& signature,
    const QString& expectedPubKey, const QString& inviteHex,
    QJSValue callback) {
  qDebug() << "[ConnectionManager] [startSessionWithPreKey] Starting session "
              "with prekey on server"
           << serverId;
  if (!this->isConnected(serverId)) {
    qDebug() << "[ConnectionManager] [startSessionWithPreKey] Not connected to "
                "server"
             << serverId;
    if (callback.isCallable()) {
      qDebug() << "[ConnectionManager] [startSessionWithPreKey] Invoking "
                  "callback with false for server"
               << serverId;
      callback.call(QJSValueList() << false);
    } else {
      qDebug() << "[ConnectionManager] [startSessionWithPreKey] No valid "
                  "callback provided for server"
               << serverId;
    }
    qDebug() << "[ConnectionManager] [startSessionWithPreKey] Session start "
                "failed for server"
             << serverId;
    return false;
  }

  qDebug() << "[ConnectionManager] [startSessionWithPreKey] Starting session "
              "on server"
           << serverId << "with" << ed_pub;

  std::vector<uint8_t> prekeyIdVec =
      coreutils::hex_to_bytes(prekeyId.toStdString());
  std::vector<uint8_t> dhPubVec = coreutils::hex_to_bytes(dh_pub.toStdString());
  std::vector<uint8_t> pqPubVec = coreutils::hex_to_bytes(pq_pub.toStdString());
  std::vector<uint8_t> edPubVec = coreutils::hex_to_bytes(ed_pub.toStdString());
  std::vector<uint8_t> signatureVec =
      coreutils::hex_to_bytes(signature.toStdString());

  // The whole point of signing prekeys is to not have to trust the server for
  // identity binding. verify_prekey() below only proves internal
  // self-consistency (signed by whoever's key is embedded in the bundle); it
  // does not prove that key belongs to the contact the caller actually asked
  // for. A malicious/compromised server could otherwise substitute its own
  // (self-consistently signed) bundle and this would silently establish E2E
  // with the attacker instead of the intended contact.
  if (ed_pub.compare(expectedPubKey, Qt::CaseInsensitive) != 0) {
    qWarning() << "[ConnectionManager] [startSessionWithPreKey] Prekey "
                  "identity mismatch: expected"
               << expectedPubKey << "got" << ed_pub
               << "— refusing to establish session for server" << serverId;
    if (callback.isCallable()) {
      callback.call(QJSValueList() << false);
    }
    return false;
  }

  prekeys::PreKey prekey;
  prekey.id = prekeyIdVec;
  prekey.dh_pub = dhPubVec;
  prekey.pq_pub = pqPubVec;
  prekey.Ed25519_pub = edPubVec;
  prekey.signature = signatureVec;

  if (!prekeys::verify_prekey(prekey)) {
    qWarning() << "[ConnectionManager] [startSessionWithPreKey] Prekey "
                  "verification failed for server"
               << serverId;
    if (callback.isCallable()) {
      callback.call(QJSValueList() << false);
    }
    return false;
  }

  // The contact's invite secret seeds the initial root key. Without the right
  // invite the receiver's ratchet diverges and nothing we send ever decrypts,
  // so refuse to start a session that is dead on arrival.
  const QByteArray inviteSecret =
      QByteArray::fromHex(inviteHex.trimmed().toUtf8());
  if (inviteSecret.size() != inviteutils::kInviteSecretSize) {
    qWarning() << "[ConnectionManager] [startSessionWithPreKey] Invalid "
                  "invite secret, aborting session start for server"
               << serverId;
    if (callback.isCallable()) {
      callback.call(QJSValueList() << false);
    }
    return false;
  }

  dr::DoubleRatchet doubleRatchet;
  pqdh::KeyPairs pqdhLocal = pqdh::generate_keypairs();

  const dr::Key32 initial_root_key = inviteutils::deriveRootKey(inviteSecret);
  std::vector<uint8_t> encapsulatedCiphertext;
  encapsulatedCiphertext = doubleRatchet.pq_init(initial_root_key, pqdhLocal,
                                                 dhPubVec, pqPubVec, true);

  qDebug() << "[ConnectionManager] [startSessionWithPreKey] Encapsulated "
              "ciphertext size:"
           << encapsulatedCiphertext.size();

  spdlog::set_level(spdlog::level::debug);
  doubleRatchet.debug_print();

  qDebug() << "[ConnectionManager] [startSessionWithPreKey] Session start "
              "requested from server"
           << serverId;

  m_storage->storeAnonymousContactSession(serverId, ed_pub, doubleRatchet);

  this->e2eSendSessionRequest(serverId, ed_pub, pqdhLocal, prekey.id,
                              encapsulatedCiphertext);

  m_contactsModel->addAnonymousContact(
      serverId, ed_pub,
      QString::fromStdString(doubleRatchet.get_session_key_fingerprint()),
      QString());

  if (callback.isCallable()) {
    callback.call(QJSValueList() << true);
  }

  return true;
}

void ConnectionManager::e2eSendSessionRequest(
    const QString& serverId, const QString& toPubKey,
    const pqdh::KeyPairs& ephemeralKeys, const std::vector<uint8_t>& prekeyId,
    const std::vector<uint8_t>& encapsulatedCiphertext) {
  qDebug() << "[ConnectionManager] [e2eSendSessionRequest] Sending session "
              "request to server"
           << serverId << "for pubkey" << toPubKey;

  if (!this->isConnected(serverId)) {
    qDebug()
        << "[ConnectionManager] [e2eSendSessionRequest] Not connected to server"
        << serverId;
    return;
  }

  std::vector<uint8_t> plaintext;

  std::vector<uint8_t> toPubKeyVec =
      coreutils::hex_to_bytes(toPubKey.toStdString());

  // [push kind][target identity][format][session request JSON]
  plaintext.push_back(static_cast<uint8_t>(PushKind::Default));
  plaintext.insert(plaintext.end(), toPubKeyVec.begin(), toPubKeyVec.end());

  plaintext.push_back(
      static_cast<uint8_t>(dr::EncryptedMessageFormat::NOT_ENCRYPTED_JSON));

  nlohmann::json sessionRequestJson;

  sessionRequestJson["type"] = "session_request";
  sessionRequestJson["prekey_id"] = coreutils::bytes_to_hex(prekeyId);
  sessionRequestJson["pq_pub"] = coreutils::bytes_to_hex(ephemeralKeys.pq_pk);
  sessionRequestJson["dh_pub"] = coreutils::bytes_to_hex(ephemeralKeys.dh_pub);
  sessionRequestJson["encapsulated_ciphertext"] =
      coreutils::bytes_to_hex(encapsulatedCiphertext);

  QByteArray edPub = m_accountManager->getPublicKeyRaw();
  std::vector<uint8_t> edPubVec(edPub.begin(), edPub.end());

  sessionRequestJson["ed_pub"] = coreutils::bytes_to_hex(edPubVec);

  std::vector<uint8_t> toSign;
  toSign.insert(toSign.end(), prekeyId.begin(), prekeyId.end());
  toSign.insert(toSign.end(), ephemeralKeys.dh_pub.begin(),
                ephemeralKeys.dh_pub.end());
  toSign.insert(toSign.end(), ephemeralKeys.pq_pk.begin(),
                ephemeralKeys.pq_pk.end());
  toSign.insert(toSign.end(), edPubVec.begin(), edPubVec.end());
  toSign.insert(toSign.end(), toPubKeyVec.begin(), toPubKeyVec.end());
  toSign.insert(toSign.end(), encapsulatedCiphertext.begin(),
                encapsulatedCiphertext.end());

  std::vector<uint8_t> privKeyVec;
  {
    QByteArray privKey = m_accountManager->getPrivateKey();
    privKeyVec = std::vector<uint8_t>(privKey.begin(), privKey.end());
  }

  std::vector<uint8_t> signature = CoreCrypto::sign(privKeyVec, toSign);

  sessionRequestJson["signature"] = coreutils::bytes_to_hex(signature);

  std::string sessionRequestStr = sessionRequestJson.dump();

  std::vector<uint8_t> sessionRequestVec(sessionRequestStr.begin(),
                                         sessionRequestStr.end());

  plaintext.insert(plaintext.end(), sessionRequestVec.begin(),
                   sessionRequestVec.end());

  this->request(serverId, ConnectionManager::RequestType::SEND_MESSAGE,
                QByteArray(reinterpret_cast<const char*>(plaintext.data()),
                           static_cast<int>(plaintext.size())));

  qDebug() << "[ConnectionManager] [e2eSendSessionRequest] ended session "
              "request method"
           << serverId;

  return;
}

uint32_t ConnectionManager::e2eSendMessage(const QString& serverId,
                                           const QString& toPubKey,
                                           const QByteArray& plaintext,
                                           dr::EncryptedMessageFormat format,
                                           QJSValue callback) {
  return this->e2eSendMessageWithPush(serverId, toPubKey, plaintext, format,
                                      PushKind::Default, callback);
}

uint32_t ConnectionManager::e2eSendMessageWithPush(
    const QString& serverId, const QString& toPubKey,
    const QByteArray& plaintext, dr::EncryptedMessageFormat format,
    PushKind pushKind, QJSValue callback) {
  qDebug() << "[ConnectionManager] [e2eSendMessage] Sending message to server"
           << serverId << "for pubkey" << toPubKey
           << "pushKind=" << static_cast<int>(pushKind);

  if (!this->isConnected(serverId)) {
    qDebug() << "[ConnectionManager] [e2eSendMessage] Not connected to server"
             << serverId;
    return 0;
  }

  dr::EncryptedMessage message;

  {
    QMutexLocker locker(&tdMutex_);

    dr::DoubleRatchet doubleRatchet =
        m_storage->loadDoubleRatchet(serverId, toPubKey);

    if (doubleRatchet.error_code != 0) {
      qWarning() << "[ConnectionManager] [e2eSendMessage] DoubleRatchet load "
                    "error for pubkey"
                 << toPubKey << "error_code=" << doubleRatchet.error_code;
      return 0;
    }

    message = doubleRatchet.encrypt(
        std::vector<uint8_t>(plaintext.begin(), plaintext.end()));

    m_storage->saveDoubleRatchet(serverId, toPubKey, doubleRatchet);

    std::vector<uint8_t> messageVec = message.serialize();

    std::vector<uint8_t> data;
    std::vector<uint8_t> toPubKeyVec =
        coreutils::hex_to_bytes(toPubKey.toStdString());

    // [push kind][target identity][format][serialized ratchet message]
    data.push_back(static_cast<uint8_t>(pushKind));
    data.insert(data.end(), toPubKeyVec.begin(), toPubKeyVec.end());
    data.push_back(static_cast<uint8_t>(format));
    data.insert(data.end(), messageVec.begin(), messageVec.end());

    uint32_t request_id =
        this->request(serverId, ConnectionManager::RequestType::SEND_MESSAGE,
                      QByteArray(reinterpret_cast<const char*>(data.data()),
                                 static_cast<int>(data.size())));

    callbacks_[request_id] = callback;

    return request_id;
  }
}

uint32_t ConnectionManager::e2eSendJSON(const QString& serverId,
                                        const QString& toPubKey,
                                        const QString& jsonStr,
                                        QJSValue callback) {
  qDebug() << "[ConnectionManager] [e2eSendJSON] Sending JSON message to server"
           << serverId << "for pubkey" << toPubKey;
  qDebug() << "[ConnectionManager] [e2eSendJSON] JSON content:" << jsonStr;

  return this->e2eSendMessage(serverId, toPubKey,
                              QByteArray::fromStdString(jsonStr.toStdString()),
                              dr::EncryptedMessageFormat::JSON, callback);
}

uint32_t ConnectionManager::e2eSendJsonWithPush(const QString& serverId,
                                                const QString& toPubKey,
                                                const QString& jsonStr,
                                                PushKind pushKind) {
  return this->e2eSendMessageWithPush(
      serverId, toPubKey, QByteArray::fromStdString(jsonStr.toStdString()),
      dr::EncryptedMessageFormat::JSON, pushKind, QJSValue());
}

uint32_t ConnectionManager::e2eDeleteMessage(const QString& serverId,
                                             const QString& toPubKey,
                                             quint64 message_id,
                                             QJSValue callback) {
  qDebug() << "[ConnectionManager] [e2eDeleteMessage] Sending delete message "
              "request to server"
           << serverId << "for pubkey" << toPubKey << "messageId"
           << static_cast<uint64_t>(message_id);

  nlohmann::json deleteJson;
  deleteJson["type"] = "delete_message";
  deleteJson["message_id"] = static_cast<uint64_t>(message_id);

  QString deleteStr = QString::fromStdString(deleteJson.dump());

  return this->e2eSendJSON(serverId, toPubKey, deleteStr, callback);
}

void ConnectionManager::sendReadedSignal(const QString& serverId,
                                         const QString& toPubKey) {
  qDebug() << "[ConnectionManager] [sendReadedSignal] Sending readed signal to "
              "server"
           << serverId << "for pubkey" << toPubKey;

  QByteArray payload;

  payload.push_back(static_cast<uint8_t>(RequestType::READED_SIGNAL));
  payload.append(QByteArray::fromHex(toPubKey.toUtf8()));

  this->sendBinaryMessage(serverId, payload);

  // this->e2eSendJSON(serverId, toPubKey, QString::fromStdString(R"({"type":
  // "readed"})"));
}

void ConnectionManager::sendReadSignal(const QString& serverId,
                                       const QString& toPubKey) {
  sendReadedSignal(serverId, toPubKey);
}

void ConnectionManager::sendSessionRemoveSignal(const QString& serverId,
                                                const QString& toPubKey) {
  qDebug() << "[ConnectionManager] [sendSessionRemoveSignal] Sending "
              "remove-chat signal to server"
           << serverId << "for pubkey" << toPubKey;

  QByteArray payload;
  payload.push_back(static_cast<uint8_t>(RequestType::SESSION_REMOVE_SIGNAL));
  payload.append(QByteArray::fromHex(toPubKey.toUtf8()));

  this->sendBinaryMessage(serverId, payload);
}

uint32_t ConnectionManager::registerFcmToken(const QString& serverId,
                                             const QString& token) {
  if (token.trimmed().isEmpty()) {
    qWarning() << "[ConnectionManager] [registerFcmToken] Empty token for"
               << serverId;
    return 0;
  }

  qDebug() << "[ConnectionManager] [registerFcmToken] Registering FCM token for"
           << serverId << "tokenLen=" << token.size();

  nlohmann::json payloadJson;
  payloadJson["token"] = token.toStdString();
  const uint32_t requestId =
      request(serverId, ConnectionManager::RequestType::REGISTER_FCM_TOKEN,
              QByteArray::fromStdString(payloadJson.dump()));
  qDebug() << "[ConnectionManager] [registerFcmToken] requestId=" << requestId
           << "serverId=" << serverId;
  return requestId;
}

void ConnectionManager::sendReceivedSignalForMessage(const QString& serverId,
                                                     const QString& fromPubKey,
                                                     quint64 messageId) {
  std::vector<uint8_t> fromVec =
      coreutils::hex_to_bytes(fromPubKey.toStdString());
  QByteArray fromArray = qutils::from_vector(fromVec);

  QByteArray payload;
  payload.push_back(static_cast<uint8_t>(RequestType::RECEIVED_SIGNAL));
  payload.append(fromArray);
  payload.append(static_cast<char>((messageId >> 56) & 0xFF));
  payload.append(static_cast<char>((messageId >> 48) & 0xFF));
  payload.append(static_cast<char>((messageId >> 40) & 0xFF));
  payload.append(static_cast<char>((messageId >> 32) & 0xFF));
  payload.append(static_cast<char>((messageId >> 24) & 0xFF));
  payload.append(static_cast<char>((messageId >> 16) & 0xFF));
  payload.append(static_cast<char>((messageId >> 8) & 0xFF));
  payload.append(static_cast<char>(messageId & 0xFF));

  this->sendBinaryMessage(serverId, payload);
}

void ConnectionManager::enrollMember(const QString& serverId,
                                     const QString& contactPubKey) {
  if (!this->isConnected(serverId)) {
    return;
  }

  const std::vector<uint8_t> pubVec =
      coreutils::hex_to_bytes(contactPubKey.toStdString());
  if (pubVec.size() != 32) {
    qWarning() << "[ConnectionManager] [enrollMember] bad pubkey length for"
               << contactPubKey;
    return;
  }

  const uint32_t requestId = this->request(
      serverId, ConnectionManager::RequestType::ENROLL_MEMBER,
      QByteArray(reinterpret_cast<const char*>(pubVec.data()),
                static_cast<int>(pubVec.size())));
  if (requestId != 0) {
    m_pendingEnrollRequests.insert(requestId, {serverId, contactPubKey});
  }
}

void ConnectionManager::retryPendingMemberEnrollments(
    const QString& serverId) {
  if (m_storage == nullptr || !isConnected(serverId) ||
      !isAuthenticated(serverId)) {
    return;
  }

  const QStringList unenrolled =
      m_storage->loadUnenrolledContactPubKeys(serverId);
  for (const QString& contactPubKey : unenrolled) {
    enrollMember(serverId, contactPubKey);
  }
}

void ConnectionManager::handleEnrollAcked(const QString& serverId,
                                          uint32_t requestId) {
  const auto it = m_pendingEnrollRequests.constFind(requestId);
  if (it == m_pendingEnrollRequests.constEnd()) {
    return;
  }

  const QString contactPubKey = it.value().second;
  m_pendingEnrollRequests.erase(it);

  if (m_storage != nullptr) {
    m_storage->markContactEnrolled(serverId, contactPubKey);
  }
}

QString ConnectionManager::buildOwnAccountInfoJson(bool isResponse) const {
  nlohmann::json responseJson;
  responseJson["type"] = "accountInfo";
  responseJson["is_response"] = isResponse;
  responseJson["firstName"] =
      m_settings->getTextSetting("firstName").toStdString();
  responseJson["lastName"] =
      m_settings->getTextSetting("lastName").toStdString();
  responseJson["aboutMe"] = m_settings->getTextSetting("aboutMe").toStdString();
  responseJson["nameStyle"] =
      m_settings->getTextSetting("nameStyle").toStdString();
  responseJson["avatarBlob"] =
      QString::fromUtf8(m_settings->getBlobSetting("avatar").toBase64())
          .toStdString();
  return QString::fromStdString(responseJson.dump());
}

void ConnectionManager::translateAccountInfo() {
  const QList<QPair<QString, QString>> endpoints =
      m_contactsModel->contactEndpoints();

  for (const auto& endpoint : endpoints) {
    const QString& serverAddress = endpoint.first;
    const QString& pubkeyFingerprint = endpoint.second;

    // accountInfo is a profile sync (name/lastName/avatar/etc.), not a message:
    // tag it Silent so an offline peer stores it without a spurious "new
    // message" push.
    this->e2eSendJsonWithPush(serverAddress, pubkeyFingerprint,
                              buildOwnAccountInfoJson(false),
                              PushKind::Silent);
  }
}

void ConnectionManager::revokeIdentity() {
  QByteArray edPub = m_accountManager->getPublicKeyRaw();
  std::vector<uint8_t> edPubVec(edPub.begin(), edPub.end());

  // Domain-separated message the certificate signs. MUST stay byte-identical to
  // protocol.RevocationSigningMessage on the server.
  static const QByteArray kRevocationContext =
      QByteArrayLiteral("PATRONUS_KEY_REVOCATION_v1");
  std::vector<uint8_t> toSign(kRevocationContext.begin(),
                              kRevocationContext.end());
  toSign.insert(toSign.end(), edPubVec.begin(), edPubVec.end());

  std::vector<uint8_t> signature;
  {
    QByteArray privKey = m_accountManager->getPrivateKey();
    std::vector<uint8_t> privKeyVec(privKey.begin(), privKey.end());
    signature = CoreCrypto::sign(privKeyVec, toSign);
  }

  const QByteArray signatureBytes(
      reinterpret_cast<const char*>(signature.data()),
      static_cast<int>(signature.size()));

  // 1) Submit the tombstone to every server we are connected to. The server
  //    re-verifies the signature against our authenticated identity, records a
  //    permanent revocation, drops our prekeys, and thereafter blocks new
  //    sessions and warns anyone who messages us.
  const QStringList servers = getConnectedServers();
  for (const QString& serverId : servers) {
    request(serverId, RequestType::REVOKE_IDENTITY, signatureBytes);
  }

  // 2) Proactively warn everyone we have a ratchet with, in-band. The notice is
  //    itself signed so a recipient can verify it independently of the session.
  //    It is a control message, so it goes out Silent (no "new message" push).
  nlohmann::json revokeJson;
  revokeJson["type"] = "identity_revoked";
  revokeJson["identity"] = coreutils::bytes_to_hex(edPubVec);
  revokeJson["signature"] = coreutils::bytes_to_hex(signature);
  const QString revokeStr = QString::fromStdString(revokeJson.dump());

  const QList<QPair<QString, QString>> endpoints =
      m_contactsModel->contactEndpoints();
  for (const auto& endpoint : endpoints) {
    e2eSendJsonWithPush(endpoint.first, endpoint.second, revokeStr,
                        PushKind::Silent);
  }

  qWarning() << "[ConnectionManager] [revokeIdentity] published key revocation to"
             << servers.size() << "server(s) and" << endpoints.size()
             << "contact(s)";
}

void ConnectionManager::e2eCallRequest(const QString& serverId,
                                       const QString& toPubKey,
                                       quint64 callId) {
  qDebug()
      << "[ConnectionManager] [e2eCallRequest] Sending call request to server"
      << serverId << "for pubkey" << toPubKey;

  prekeys::PreKey prekey = m_accountManager->generatePreKey(true);

  nlohmann::json callRequestJson;
  callRequestJson["type"] = "call_request";
  callRequestJson["timestamp"] =
      QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
  callRequestJson["dh_pub"] = coreutils::bytes_to_hex(prekey.dh_pub);
  callRequestJson["id"] = coreutils::bytes_to_hex(prekey.id);
  callRequestJson["callId"] = static_cast<uint64_t>(callId);

  // Tagged as a call so the server wakes an offline peer with a data-only call
  // push (instead of a plain "new message" notification).
  this->e2eSendJsonWithPush(serverId, toPubKey,
                            QString::fromStdString(callRequestJson.dump()),
                            PushKind::Call);

  return;
}

void ConnectionManager::e2eCallDiscard(const QString& serverId,
                                       const QString& toPubKey,
                                       const uint64_t callId,
                                       const QString& reason) {
  qDebug()
      << "[ConnectionManager] [e2eCallDiscard] Sending call discard to server"
      << serverId << "for pubkey" << toPubKey;

  nlohmann::json callDiscardJson;
  callDiscardJson["type"] = "call_discard";
  callDiscardJson["callId"] = static_cast<uint64_t>(callId);
  // Only present for special teardowns (e.g. "busy"); a normal hang-up omits it.
  if (!reason.isEmpty()) {
    callDiscardJson["reason"] = reason.toStdString();
  }

  // Stored-only if the peer is offline: never raise a push for a call teardown.
  this->e2eSendJsonWithPush(serverId, toPubKey,
                            QString::fromStdString(callDiscardJson.dump()),
                            PushKind::Silent);

  return;
}

void ConnectionManager::e2eCallCancel(const QString& serverId,
                                      const QString& toPubKey,
                                      const uint64_t callId) {
  qDebug()
      << "[ConnectionManager] [e2eCallCancel] Sending call cancel to server"
      << serverId << "for pubkey" << toPubKey;

  nlohmann::json callCancelJson;
  callCancelJson["type"] = "call_cancel";
  callCancelJson["callId"] = static_cast<uint64_t>(callId);

  // If the peer is offline it was likely woken and is ringing via a call push;
  // send a data-only call_cancel push so that ring stops and the notification is
  // dismissed.
  this->e2eSendJsonWithPush(serverId, toPubKey,
                            QString::fromStdString(callCancelJson.dump()),
                            PushKind::CallCancel);

  return;
}

void ConnectionManager::e2eCallAccept(const QString& serverId,
                                      const QString& toPubKey,
                                      const std::vector<uint8_t>& dh_pub,
                                      const std::vector<uint8_t>& id,
                                      uint64_t callId) {
  qDebug()
      << "[ConnectionManager] [e2eCallAccept] Sending call accept to server"
      << serverId << "for pubkey" << toPubKey;

  nlohmann::json callAcceptJson;
  callAcceptJson["type"] = "call_accept";
  callAcceptJson["dh_pub"] = coreutils::bytes_to_hex(dh_pub);
  callAcceptJson["id"] = coreutils::bytes_to_hex(id);
  callAcceptJson["callId"] = callId;

  // Accept always travels to an online peer (they are ringing); tag Silent so a
  // rare offline race stores it without a spurious push.
  this->e2eSendJsonWithPush(serverId, toPubKey,
                            QString::fromStdString(callAcceptJson.dump()),
                            PushKind::Silent);

  return;
}

QString ConnectionManager::getCallKeysFingerprint() {
  std::vector<uint8_t> key1 = m_callKeys.send_key;
  std::vector<uint8_t> key2 = m_callKeys.receive_key;

  std::vector<uint8_t> combined;
  combined.reserve(key1.size() + key2.size());
  combined.insert(combined.end(), key1.begin(), key1.end());
  combined.insert(combined.end(), key2.begin(), key2.end());

  // Sort to keep the result invariant to input order
  std::sort(combined.begin(), combined.end());

  QByteArray hash = QCryptographicHash::hash(
      QByteArray(reinterpret_cast<const char*>(combined.data()),
                 static_cast<int>(combined.size())),
      QCryptographicHash::Sha256);

  QStringList groups;
  for (int g = 0; g < 2; ++g) {
    QString group;
    for (int i = 0; i < 4; ++i) {
      group += QString::number(hash[g * 4 + i] & 0xFF).right(1);
      if (i != 3) group += " ";
    }
    groups << group;
  }

  // Build a string like: "0 0 0 0  0 0 0 0"
  return groups[0] + "  " + groups[1];
}

void ConnectionManager::setCallSessionState(
    const CoreCrypto::SessionKeys& sessionKeys, const QString& serverId,
    const QString& contactPubKey) {
  m_callKeys = sessionKeys;
  m_currentContactServer = serverId;
  m_currentContactPubKey = contactPubKey;
}

CoreCrypto::SessionKeys ConnectionManager::callSessionKeys() const {
  return m_callKeys;
}

QString ConnectionManager::currentCallServer() const {
  return m_currentContactServer;
}

QString ConnectionManager::currentCallContactPubKey() const {
  return m_currentContactPubKey;
}

void ConnectionManager::sendAudioFrame(const QString& serverId,
                                       const QString& toPubKey,
                                       const QByteArray& encryptedFrame) {
  if (!this->isConnected(serverId)) {
    qDebug() << "[ConnectionManager] [sendAudioFrame] Not connected to server"
             << serverId;
    return;
  }

  // Format: [format byte = AUDIO_FRAME][to_pubkey 32 bytes][encrypted frame]
  std::vector<uint8_t> toPubKeyVec =
      coreutils::hex_to_bytes(toPubKey.toStdString());

  QByteArray data;
  data.append(static_cast<uint8_t>(RequestType::AUDIO_FRAME));
  data.append(reinterpret_cast<const char*>(toPubKeyVec.data()),
              static_cast<int>(toPubKeyVec.size()));

  data.append(encryptedFrame);

  this->sendBinaryMessage(serverId, data);
}

void ConnectionManager::onOnlineRequest(const QString& serverAddress,
                                        const QString& contactPubKey) {
  // qDebug() << "[ConnectionManager] [onOnlineRequest] Online status request
  // sent to server" << serverAddress << "for contactPubKey" << contactPubKey;

  if (!this->isConnected(serverAddress)) {
    qDebug() << "[ConnectionManager] [onOnlineRequest] Not connected to server"
             << serverAddress;
    return;
  }

  this->isOnline(serverAddress, contactPubKey);
}

void ConnectionManager::onMessageDelete(const quint64 message_id,
                                        const QString& serverId,
                                        const QString& contactPubKey) {
  qDebug() << "[ConnectionManager] [onMessageDelete] Delete message request "
              "sent for message_id"
           << message_id << "and contactPubKey" << contactPubKey;

  if (!m_contactsModel->hasContact(serverId, contactPubKey)) {
    qWarning()
        << "[ConnectionManager] [onMessageDelete] No contact found for pubkey"
        << contactPubKey;
    return;
  }

  this->e2eDeleteMessage(serverId, contactPubKey, message_id);
}

void ConnectionManager::onContactRemove(const QString& serverId,
                                        const QString& contactPubKey) {
  qDebug() << "[ConnectionManager] [onContactRemove] Contact remove request "
              "sent for contactPubKey"
           << contactPubKey << "on server" << serverId;

  sendSessionRemoveSignal(serverId, contactPubKey);
}

void ConnectionManager::onClearHistory(const QString& serverId,
                                       const QString& contactPubKey) {
  qDebug() << "[ConnectionManager] [onClearHistory] Clear history request sent "
              "for contactPubKey"
           << contactPubKey << "on server" << serverId;

  nlohmann::json clearJson;
  clearJson["type"] = "clear_history";

  this->e2eSendJSON(serverId, contactPubKey,
                    QString::fromStdString(clearJson.dump()));
}

void ConnectionManager::onSetTimer(const QString& serverId,
                                   const QString& contactPubKey, int seconds) {
  qDebug() << "[ConnectionManager] [onSetTimer] Set timer request sent for "
              "contactPubKey"
           << contactPubKey << "on server" << serverId << "with seconds"
           << seconds;

  nlohmann::json timerJson;
  timerJson["type"] = "set_timer";
  timerJson["seconds"] = seconds;

  this->e2eSendJSON(serverId, contactPubKey,
                    QString::fromStdString(timerJson.dump()));
}