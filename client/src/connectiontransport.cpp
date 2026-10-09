#include "connectiontransport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUrl>

#include "appversion.h"

namespace {

constexpr auto kProtocolVersionHeader = "X-Protocol-Version";
constexpr auto kProtocolVersion = appversion::kProtocolVersion;

}  // namespace

ConnectionTransport::ConnectionTransport(QObject* parent)
    : QObject(parent),
      reconnectTimer_(new QTimer(this)),
      pingTimer_(new QTimer(this)),
      reconnectInterval_(5000),
      maxReconnectAttempts_(-1),
      connectionTimeout_(10000),
      pingInterval_(10000),
      deadConnectionTimeout_(5000) {
  connect(reconnectTimer_, &QTimer::timeout, this,
          &ConnectionTransport::checkConnections);
  reconnectTimer_->start(reconnectInterval_);

  connect(pingTimer_, &QTimer::timeout, this,
          &ConnectionTransport::checkConnections);
  pingTimer_->start(1000);
}

ConnectionTransport::~ConnectionTransport() { removeAllServers(); }

bool ConnectionTransport::addServer(const QString& serverId,
                                    const QString& serverUrl) {
  if (QThread::currentThread() == thread()) {
    return addServerImpl(serverId, serverUrl);
  }

  bool result = false;
  QMetaObject::invokeMethod(
      this,
      [this, &result, serverId, serverUrl]() {
        result = addServerImpl(serverId, serverUrl);
      },
      Qt::BlockingQueuedConnection);
  return result;
}

bool ConnectionTransport::addServerImpl(const QString& serverId,
                                        const QString& serverUrl) {
  QMutexLocker locker(&mutex_);
  if (connections_.contains(serverId)) {
    return false;
  }

  auto conn = std::make_shared<ServerConnection>();
  conn->serverId = serverId;
  conn->serverUrl = QStringLiteral("wss://") + serverUrl;

  qWarning() << "[ConnectionTransport] Creating connection for server:"
           << conn->serverUrl;

  conn->socket =
      new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);

  QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
  sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
  sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
  conn->socket->setSslConfiguration(sslConfig);

  connect(conn->socket, &QWebSocket::binaryMessageReceived, this,
          &ConnectionTransport::onBinaryMessageReceived);
  connect(conn->socket, &QWebSocket::textMessageReceived, this,
          &ConnectionTransport::onTextMessageReceived);
  connect(conn->socket, &QWebSocket::disconnected, this,
          &ConnectionTransport::onDisconnected);
  connect(conn->socket, &QWebSocket::connected, this,
          &ConnectionTransport::onConnected);
  connect(conn->socket, &QWebSocket::errorOccurred, this,
          &ConnectionTransport::onError);
  connect(conn->socket, &QWebSocket::sslErrors, this,
          &ConnectionTransport::onSslErrors);
  connect(conn->socket, &QWebSocket::pong, this, &ConnectionTransport::onPong);

  conn->timeoutTimer = new QTimer(this);
  conn->timeoutTimer->setSingleShot(true);
  connect(conn->timeoutTimer, &QTimer::timeout, this,
          &ConnectionTransport::onConnectionTimeout);

  connections_[serverId] = conn;
  connectToServer(conn.get());
  return true;
}

bool ConnectionTransport::hasServer(const QString& serverId) const {
  if (QThread::currentThread() == thread()) {
    return hasServerImpl(serverId);
  }

  bool result = false;
  QMetaObject::invokeMethod(
      const_cast<ConnectionTransport*>(this),
      [this, &result, serverId]() { result = hasServerImpl(serverId); },
      Qt::BlockingQueuedConnection);
  return result;
}

bool ConnectionTransport::hasServerImpl(const QString& serverId) const {
  QMutexLocker locker(&mutex_);
  return connections_.contains(serverId);
}

bool ConnectionTransport::connectServer(const QString& serverId) {
  if (QThread::currentThread() == thread()) {
    return connectServerImpl(serverId);
  }

  bool result = false;
  QMetaObject::invokeMethod(
      this,
      [this, &result, serverId]() { result = connectServerImpl(serverId); },
      Qt::BlockingQueuedConnection);
  return result;
}

bool ConnectionTransport::connectServerImpl(const QString& serverId) {
  QMutexLocker locker(&mutex_);
  auto it = connections_.find(serverId);
  if (it == connections_.end()) {
    return false;
  }

  connectToServer(it.value().get());
  return true;
}

bool ConnectionTransport::removeServer(const QString& serverId) {
  if (QThread::currentThread() == thread()) {
    return removeServerImpl(serverId);
  }

  bool result = false;
  QMetaObject::invokeMethod(
      this,
      [this, &result, serverId]() { result = removeServerImpl(serverId); },
      Qt::BlockingQueuedConnection);
  return result;
}

bool ConnectionTransport::removeServerImpl(const QString& serverId) {
  QMutexLocker locker(&mutex_);
  auto it = connections_.find(serverId);
  if (it == connections_.end()) {
    return false;
  }

  auto conn = it.value();
  if (conn->socket) {
    conn->socket->abort();
    conn->socket->deleteLater();
  }
  if (conn->timeoutTimer) {
    conn->timeoutTimer->stop();
    conn->timeoutTimer->deleteLater();
  }

  connections_.erase(it);
  return true;
}

void ConnectionTransport::removeAllServers() {
  if (QThread::currentThread() == thread()) {
    removeAllServersImpl();
    return;
  }

  QMetaObject::invokeMethod(
      this, [this]() { removeAllServersImpl(); }, Qt::BlockingQueuedConnection);
}

void ConnectionTransport::removeAllServersImpl() {
  QMutexLocker locker(&mutex_);
  for (auto& conn : connections_) {
    if (conn->socket) {
      conn->socket->abort();
      conn->socket->deleteLater();
    }
    if (conn->timeoutTimer) {
      conn->timeoutTimer->stop();
      conn->timeoutTimer->deleteLater();
    }
  }
  connections_.clear();
}

bool ConnectionTransport::sendTextMessage(const QString& serverId,
                                          const QString& message) {
  if (QThread::currentThread() == thread()) {
    return sendTextMessageImpl(serverId, message);
  }

  bool result = false;
  QMetaObject::invokeMethod(
      this,
      [this, &result, serverId, message]() {
        result = sendTextMessageImpl(serverId, message);
      },
      Qt::BlockingQueuedConnection);
  return result;
}

bool ConnectionTransport::sendTextMessageImpl(const QString& serverId,
                                              const QString& message) {
  QMutexLocker locker(&mutex_);
  auto it = connections_.find(serverId);
  if (it == connections_.end()) {
    qWarning() << "[ConnectionTransport] Server not found:" << serverId;
    return false;
  }

  auto conn = it.value();
  if (conn->status != ConnectionStatus::Connected || !conn->socket) {
    qWarning() << "[ConnectionTransport] Server not connected:" << serverId;
    return false;
  }

  qint64 bytesSent = conn->socket->sendTextMessage(message);
  if (bytesSent > 0) {
    qDebug() << "[ConnectionTransport] Message sent to" << serverId << ':'
             << bytesSent << "bytes";
    return true;
  }

  qWarning() << "[ConnectionTransport] Failed to send message to" << serverId;
  return false;
}

bool ConnectionTransport::sendBinaryMessage(const QString& serverId,
                                            const QByteArray& data) {
  if (QThread::currentThread() == thread()) {
    return sendBinaryMessageImpl(serverId, data);
  }

  bool result = false;
  QMetaObject::invokeMethod(
      this,
      [this, &result, serverId, data]() {
        result = sendBinaryMessageImpl(serverId, data);
      },
      Qt::BlockingQueuedConnection);
  return result;
}

bool ConnectionTransport::sendBinaryMessageImpl(const QString& serverId,
                                                const QByteArray& data) {
  QMutexLocker locker(&mutex_);
  auto it = connections_.find(serverId);
  if (it == connections_.end()) {
    qWarning() << "[ConnectionTransport] Server not found:" << serverId;
    return false;
  }

  auto conn = it.value();
  if (conn->status != ConnectionStatus::Connected || !conn->socket) {
    qWarning() << "[ConnectionTransport] Server not connected:" << serverId;
    return false;
  }

  qint64 bytesSent = conn->socket->sendBinaryMessage(data);
  if (bytesSent > 0) {
    return true;
  }

  qWarning() << "[ConnectionTransport] Failed to send binary message to"
             << serverId;
  return false;
}

void ConnectionTransport::setReconnectBlocked(const QString& serverId,
                                              bool blocked) {
  if (QThread::currentThread() == thread()) {
    setReconnectBlockedImpl(serverId, blocked);
    return;
  }

  QMetaObject::invokeMethod(
      this,
      [this, serverId, blocked]() {
        setReconnectBlockedImpl(serverId, blocked);
      },
      Qt::BlockingQueuedConnection);
}

void ConnectionTransport::setReconnectBlockedImpl(const QString& serverId,
                                                  bool blocked) {
  QMutexLocker locker(&mutex_);
  auto it = connections_.find(serverId);
  if (it == connections_.end()) {
    return;
  }

  auto conn = it.value();
  conn->reconnectBlocked = blocked;
  if (blocked && conn->status == ConnectionStatus::Reconnecting) {
    updateConnectionStatus(conn.get(), ConnectionStatus::Error);
  }
}

bool ConnectionTransport::isConnected(const QString& serverId) const {
  if (QThread::currentThread() == thread()) {
    return isConnectedImpl(serverId);
  }

  bool result = false;
  QMetaObject::invokeMethod(
      const_cast<ConnectionTransport*>(this),
      [this, &result, serverId]() { result = isConnectedImpl(serverId); },
      Qt::BlockingQueuedConnection);
  return result;
}

bool ConnectionTransport::isConnectedImpl(const QString& serverId) const {
  QMutexLocker locker(&mutex_);
  auto it = connections_.find(serverId);
  return it != connections_.end() &&
         it.value()->status == ConnectionStatus::Connected;
}

QString ConnectionTransport::httpsBaseUrl(const QString& serverId) const {
  if (QThread::currentThread() == thread()) {
    return httpsBaseUrlImpl(serverId);
  }

  QString result;
  QMetaObject::invokeMethod(
      const_cast<ConnectionTransport*>(this),
      [this, &result, serverId]() { result = httpsBaseUrlImpl(serverId); },
      Qt::BlockingQueuedConnection);
  return result;
}

QString ConnectionTransport::httpsBaseUrlImpl(const QString& serverId) const {
  QMutexLocker locker(&mutex_);
  auto it = connections_.find(serverId);
  if (it == connections_.end()) {
    return QString();
  }

  QString wsUrl = it.value()->serverUrl;
  if (wsUrl.startsWith(QStringLiteral("wss://"))) {
    return QStringLiteral("https://") +
           wsUrl.mid(QStringLiteral("wss://").size());
  }
  if (wsUrl.startsWith(QStringLiteral("ws://"))) {
    return QStringLiteral("http://") +
           wsUrl.mid(QStringLiteral("ws://").size());
  }

  return wsUrl;
}

ConnectionTransport::ConnectionStatus ConnectionTransport::status(
    const QString& serverId) const {
  if (QThread::currentThread() == thread()) {
    return statusImpl(serverId);
  }

  ConnectionStatus result = ConnectionStatus::Disconnected;
  QMetaObject::invokeMethod(
      const_cast<ConnectionTransport*>(this),
      [this, &result, serverId]() { result = statusImpl(serverId); },
      Qt::BlockingQueuedConnection);
  return result;
}

ConnectionTransport::ConnectionStatus ConnectionTransport::statusImpl(
    const QString& serverId) const {
  QMutexLocker locker(&mutex_);
  auto it = connections_.find(serverId);
  if (it == connections_.end()) {
    return ConnectionStatus::Disconnected;
  }
  return it.value()->status;
}

QStringList ConnectionTransport::connectedServers() const {
  if (QThread::currentThread() == thread()) {
    return connectedServersImpl();
  }

  QStringList result;
  QMetaObject::invokeMethod(
      const_cast<ConnectionTransport*>(this),
      [this, &result]() { result = connectedServersImpl(); },
      Qt::BlockingQueuedConnection);
  return result;
}

QStringList ConnectionTransport::connectedServersImpl() const {
  QMutexLocker locker(&mutex_);
  QStringList result;
  for (auto it = connections_.constBegin(); it != connections_.constEnd();
       ++it) {
    if (it.value()->status == ConnectionStatus::Connected) {
      result.append(it.key());
    }
  }
  return result;
}

QStringList ConnectionTransport::allServers() const {
  if (QThread::currentThread() == thread()) {
    return allServersImpl();
  }

  QStringList result;
  QMetaObject::invokeMethod(
      const_cast<ConnectionTransport*>(this),
      [this, &result]() { result = allServersImpl(); },
      Qt::BlockingQueuedConnection);
  return result;
}

QStringList ConnectionTransport::allServersImpl() const {
  QMutexLocker locker(&mutex_);
  return connections_.keys();
}

void ConnectionTransport::setReconnectInterval(int milliseconds) {
  if (QThread::currentThread() == thread()) {
    setReconnectIntervalImpl(milliseconds);
    return;
  }

  QMetaObject::invokeMethod(
      this, [this, milliseconds]() { setReconnectIntervalImpl(milliseconds); },
      Qt::BlockingQueuedConnection);
}

void ConnectionTransport::setReconnectIntervalImpl(int milliseconds) {
  reconnectInterval_ = milliseconds;
  reconnectTimer_->setInterval(milliseconds);
}

void ConnectionTransport::setMaxReconnectAttempts(int attempts) {
  if (QThread::currentThread() == thread()) {
    setMaxReconnectAttemptsImpl(attempts);
    return;
  }

  QMetaObject::invokeMethod(
      this, [this, attempts]() { setMaxReconnectAttemptsImpl(attempts); },
      Qt::BlockingQueuedConnection);
}

void ConnectionTransport::setMaxReconnectAttemptsImpl(int attempts) {
  maxReconnectAttempts_ = attempts;
}

void ConnectionTransport::setConnectionTimeout(int milliseconds) {
  if (QThread::currentThread() == thread()) {
    setConnectionTimeoutImpl(milliseconds);
    return;
  }

  QMetaObject::invokeMethod(
      this, [this, milliseconds]() { setConnectionTimeoutImpl(milliseconds); },
      Qt::BlockingQueuedConnection);
}

void ConnectionTransport::setConnectionTimeoutImpl(int milliseconds) {
  connectionTimeout_ = milliseconds;
}

void ConnectionTransport::setPingInterval(int milliseconds) {
  if (QThread::currentThread() == thread()) {
    setPingIntervalImpl(milliseconds);
    return;
  }

  QMetaObject::invokeMethod(
      this, [this, milliseconds]() { setPingIntervalImpl(milliseconds); },
      Qt::BlockingQueuedConnection);
}

void ConnectionTransport::setPingIntervalImpl(int milliseconds) {
  pingInterval_ = milliseconds;
}

void ConnectionTransport::setDeadConnectionTimeout(int milliseconds) {
  if (QThread::currentThread() == thread()) {
    setDeadConnectionTimeoutImpl(milliseconds);
    return;
  }

  QMetaObject::invokeMethod(
      this,
      [this, milliseconds]() { setDeadConnectionTimeoutImpl(milliseconds); },
      Qt::BlockingQueuedConnection);
}

void ConnectionTransport::setDeadConnectionTimeoutImpl(int milliseconds) {
  deadConnectionTimeout_ = milliseconds;
}

void ConnectionTransport::setSyncInProgress(const QString& serverId,
                                            bool inProgress) {
  if (QThread::currentThread() == thread()) {
    setSyncInProgressImpl(serverId, inProgress);
    return;
  }

  // Non-blocking: the higher layer flips this from the main thread while it is
  // busy draining the sync stream, and it must not stall waiting on the
  // transport thread (which may itself be servicing that stream).
  QMetaObject::invokeMethod(
      this,
      [this, serverId, inProgress]() {
        setSyncInProgressImpl(serverId, inProgress);
      },
      Qt::QueuedConnection);
}

void ConnectionTransport::setSyncInProgressImpl(const QString& serverId,
                                                bool inProgress) {
  QMutexLocker locker(&mutex_);
  auto it = connections_.find(serverId);
  if (it == connections_.end()) {
    return;
  }

  auto conn = it.value();
  conn->syncInProgress = inProgress;
  // Re-arm the window from "now" so a stale lastAliveAt from before the sync
  // cannot trip the (widened or narrowed) watchdog on the very next tick.
  conn->lastAliveAt = QDateTime::currentDateTime();
  qWarning() << "[ConnectionTransport] syncInProgress =" << inProgress << "for"
             << serverId;
}

void ConnectionTransport::onConnected() {
  QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
  if (!socket) {
    return;
  }

  QMutexLocker locker(&mutex_);
  ServerConnection* conn = findConnectionBySocket(socket);
  if (!conn) {
    return;
  }

  qWarning() << "[ConnectionTransport] Connected to server:" << conn->serverId;
  cancelConnectionTimeout(conn);
  conn->reconnectBlocked = false;
  conn->reconnectAttempts = 0;
  conn->pingPending = false;
  conn->lastPingSentAt = QDateTime();
  conn->lastAliveAt = QDateTime::currentDateTime();
  // Start every fresh connection in the tight-watchdog regime; the server always
  // opens the session with a sync, which re-arms syncInProgress via SYNC_START.
  conn->syncInProgress = false;
  updateConnectionStatus(conn, ConnectionStatus::Connected);
}

void ConnectionTransport::onDisconnected() {
  QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
  if (!socket) {
    return;
  }

  QString serverId;
  {
    QMutexLocker locker(&mutex_);
    ServerConnection* conn = findConnectionBySocket(socket);
    if (!conn) {
      return;
    }

    qWarning() << "[ConnectionTransport] Disconnected from server:"
             << conn->serverId;
    serverId = conn->serverId;
    const ConnectionStatus prevStatus = conn->status;
    cancelConnectionTimeout(conn);
    conn->pingPending = false;
    conn->lastPingSentAt = QDateTime();
    if (prevStatus == ConnectionStatus::Connected) {
      scheduleReconnect(conn);
    } else if (prevStatus != ConnectionStatus::Reconnecting) {
      updateConnectionStatus(conn, ConnectionStatus::Disconnected);
    }
  }

  emit disconnected(serverId);
}

void ConnectionTransport::onTextMessageReceived(const QString& message) {
  QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
  if (!socket) {
    return;
  }

  QString serverId;
  {
    QMutexLocker locker(&mutex_);
    ServerConnection* conn = findConnectionBySocket(socket);
    if (!conn) {
      return;
    }

    conn->lastAliveAt = QDateTime::currentDateTime();
    conn->pingPending = false;
    conn->lastPingSentAt = QDateTime();
    serverId = conn->serverId;
    qWarning() << "[ConnectionTransport] Message received from" << conn->serverId
               << ':' << message.length() << "chars";
  }

  emit textMessageReceived(serverId, message);
}

void ConnectionTransport::onBinaryMessageReceived(const QByteArray& data) {
  QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
  if (!socket) {
    return;
  }

  QString serverId;
  {
    QMutexLocker locker(&mutex_);
    ServerConnection* conn = findConnectionBySocket(socket);
    if (!conn) {
      return;
    }

    // Any inbound frame proves the link is alive — treat it exactly like a pong
    // so a busy offline-sync stream holds off the dead-connection watchdog, and
    // a stuck pingPending can never falsely tear down an idle-but-healthy link
    // afterwards.
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 gapMs =
        conn->lastAliveAt.isValid() ? conn->lastAliveAt.msecsTo(now) : -1;
    // Diagnostic (qWarning so it survives the release log filter): only flag a
    // suspicious >2s gap between inbound frames. A healthy chunk stream has
    // sub-second gaps and stays quiet here; a real stall shows up as one line.
    if (gapMs >= 2000) {
      qWarning() << "[ConnectionTransport] inbound gap" << gapMs << "ms before"
                 << data.size() << "byte frame from" << conn->serverId;
    }
    conn->lastAliveAt = now;
    conn->pingPending = false;
    conn->lastPingSentAt = QDateTime();
    serverId = conn->serverId;
  }

  emit binaryMessageReceived(serverId, data);
}

void ConnectionTransport::onError(QAbstractSocket::SocketError error) {
  qWarning() << "[ConnectionTransport] Socket error occurred:" << error;

  QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
  if (!socket) {
    return;
  }

  QString serverId;
  QString errorMsg;
  {
    QMutexLocker locker(&mutex_);
    ServerConnection* conn = findConnectionBySocket(socket);
    if (!conn) {
      return;
    }

    serverId = conn->serverId;
    errorMsg = socket->errorString();
    cancelConnectionTimeout(conn);
    conn->pingPending = false;
    conn->lastPingSentAt = QDateTime();
    updateConnectionStatus(conn, ConnectionStatus::Error);
    scheduleReconnect(conn);
  }

  emit socketErrorOccurred(serverId, errorMsg);
}

void ConnectionTransport::onSslErrors(const QList<QSslError>& errors) {
  QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
  if (!socket) {
    return;
  }

  QString serverId;
  QStringList errorMessages;
  {
    QMutexLocker locker(&mutex_);
    ServerConnection* conn = findConnectionBySocket(socket);
    if (!conn) {
      return;
    }

    serverId = conn->serverId;
    for (const QSslError& error : errors) {
      errorMessages << error.errorString();
    }
    socket->ignoreSslErrors();
  }

  for (const QString& errorMessage : errorMessages) {
    emit sslErrorOccurred(serverId, errorMessage);
  }
}

void ConnectionTransport::onPong(quint64 elapsedTime,
                                 const QByteArray& payload) {
  Q_UNUSED(payload)

  QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
  if (!socket) {
    return;
  }

  QString serverId;
  {
    QMutexLocker locker(&mutex_);
    ServerConnection* conn = findConnectionBySocket(socket);
    if (!conn) {
      return;
    }

    conn->pingPending = false;
    conn->lastPingSentAt = QDateTime();
    conn->lastAliveAt = QDateTime::currentDateTime();
    serverId = conn->serverId;
  }

  emit pongReceived(serverId, elapsedTime);
}

void ConnectionTransport::checkConnections() {
  QMutexLocker locker(&mutex_);
  const QDateTime now = QDateTime::currentDateTime();

  for (auto& conn : connections_) {
    if (conn->status == ConnectionStatus::Reconnecting) {
      if (conn->reconnectBlocked) {
        updateConnectionStatus(conn.get(), ConnectionStatus::Error);
        continue;
      }

      if (maxReconnectAttempts_ >= 0 &&
          conn->reconnectAttempts >= maxReconnectAttempts_) {
        qWarning() << "[ConnectionTransport] Max reconnect attempts reached for"
                   << conn->serverId;
        updateConnectionStatus(conn.get(), ConnectionStatus::Error);
        continue;
      }

      if (conn->lastReconnectAttempt.msecsTo(now) >= reconnectInterval_) {
        conn->reconnectAttempts++;
        conn->lastReconnectAttempt = now;
        connectToServer(conn.get());
      }
    }

    if (conn->status == ConnectionStatus::Connected) {
      // Liveness is ANY inbound activity — a pong, a whole frame, or a single
      // chunk of a large one. Big payloads are chunked upstream (see protocol
      // ChunkThreshold), so even a very slow transfer keeps refreshing this
      // every ~32 KB; the watchdog therefore only fires on a genuinely silent
      // link, never mid-download, no matter how slow the peer is.
      if (conn->lastAliveAt.isValid() &&
          conn->lastAliveAt.msecsTo(now) >= deadConnectionTimeout_) {
        qWarning() << "[ConnectionTransport] Dead connection for"
                   << conn->serverId << "- no inbound activity for"
                   << conn->lastAliveAt.msecsTo(now) << "ms (limit"
                   << deadConnectionTimeout_ << "ms)";
        conn->pingPending = false;
        conn->lastPingSentAt = QDateTime();
        scheduleReconnect(conn.get());
        if (conn->socket) {
          conn->socket->abort();
        }
        continue;
      }

      if (!conn->pingPending && conn->lastAliveAt.isValid() &&
          conn->lastAliveAt.msecsTo(now) >= pingInterval_) {
        sendPing(conn.get());
      }
    }
  }
}

void ConnectionTransport::onConnectionTimeout() {
  QTimer* timer = qobject_cast<QTimer*>(sender());
  if (!timer) {
    return;
  }

  QMutexLocker locker(&mutex_);
  ServerConnection* conn = findConnectionByTimer(timer);
  if (!conn) {
    return;
  }

  qWarning() << "[ConnectionTransport] Connection timeout for"
             << conn->serverId;
  conn->pingPending = false;
  conn->lastPingSentAt = QDateTime();
  if (conn->socket) {
    conn->socket->abort();
  }
  scheduleReconnect(conn);
}

void ConnectionTransport::connectToServer(ServerConnection* conn) {
  if (!conn || !conn->socket) {
    return;
  }

  qWarning() << "[ConnectionTransport] Connecting to server:" << conn->serverUrl;
  updateConnectionStatus(conn, ConnectionStatus::Connecting);
  startConnectionTimeout(conn);

  QNetworkRequest request(QUrl(conn->serverUrl));
  request.setRawHeader(kProtocolVersionHeader, QByteArray(kProtocolVersion));
  conn->socket->open(request);
}

void ConnectionTransport::scheduleReconnect(ServerConnection* conn) {
  if (!conn) {
    return;
  }

  if (conn->reconnectBlocked) {
    updateConnectionStatus(conn, ConnectionStatus::Error);
    qWarning() << "[ConnectionTransport] Auto reconnect blocked for"
             << conn->serverId;
    return;
  }

  conn->lastReconnectAttempt = QDateTime::currentDateTime();
  updateConnectionStatus(conn, ConnectionStatus::Reconnecting);
  qWarning() << "[ConnectionTransport] Reconnect scheduled for" << conn->serverId;
}

void ConnectionTransport::updateConnectionStatus(ServerConnection* conn,
                                                 ConnectionStatus status) {
  if (!conn || conn->status == status) {
    return;
  }

  conn->status = status;
  emit connectionStatusChanged(conn->serverId, static_cast<int>(status));
}

void ConnectionTransport::startConnectionTimeout(ServerConnection* conn) {
  if (conn && conn->timeoutTimer) {
    conn->timeoutTimer->start(connectionTimeout_);
  }
}

void ConnectionTransport::cancelConnectionTimeout(ServerConnection* conn) {
  if (conn && conn->timeoutTimer) {
    conn->timeoutTimer->stop();
  }
}

void ConnectionTransport::sendPing(ServerConnection* conn) {
  if (!conn || !conn->socket || conn->status != ConnectionStatus::Connected) {
    return;
  }

  if (conn->pingPending) {
    qWarning() << "[ConnectionTransport] Ping pending for" << conn->serverId
               << "- connection may be dead";
    return;
  }

  conn->socket->ping();
  conn->pingPending = true;
  conn->lastPingSentAt = QDateTime::currentDateTime();
  if (conn->syncInProgress) {
    qWarning() << "[ConnectionTransport] ping sent during sync to"
               << conn->serverId;
  }
}

ConnectionTransport::ServerConnection*
ConnectionTransport::findConnectionBySocket(QWebSocket* socket) const {
  for (auto it = connections_.constBegin(); it != connections_.constEnd();
       ++it) {
    if (it.value()->socket == socket) {
      return it.value().get();
    }
  }
  return nullptr;
}

ConnectionTransport::ServerConnection*
ConnectionTransport::findConnectionByTimer(QTimer* timer) const {
  for (auto it = connections_.constBegin(); it != connections_.constEnd();
       ++it) {
    if (it.value()->timeoutTimer == timer) {
      return it.value().get();
    }
  }
  return nullptr;
}