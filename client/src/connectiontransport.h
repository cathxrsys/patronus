#ifndef CONNECTIONTRANSPORT_H
#define CONNECTIONTRANSPORT_H

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QRecursiveMutex>
#include <QThread>
#include <QTimer>
#include <QWebSocket>
#include <memory>

class ConnectionTransport : public QObject {
  Q_OBJECT

 public:
  enum class ConnectionStatus {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Error
  };

  struct ServerConnection {
    QString serverId;
    QString serverUrl;
    QWebSocket* socket;
    ConnectionStatus status;
    QDateTime lastAliveAt;
    QDateTime lastPingSentAt;
    QDateTime lastReconnectAttempt;
    int reconnectAttempts;
    QTimer* timeoutTimer;
    bool pingPending;
    bool reconnectBlocked;
    // True between SYNC_START and SYNC_END. Informational only now (used for the
    // "ping sent during sync" diagnostic): liveness is kept fresh by the chunked
    // byte stream, so the watchdog no longer needs a sync-specific window.
    bool syncInProgress;

    ServerConnection()
        : socket(nullptr),
          status(ConnectionStatus::Disconnected),
          reconnectAttempts(0),
          timeoutTimer(nullptr),
          pingPending(false),
          reconnectBlocked(false),
          syncInProgress(false) {}
  };

  explicit ConnectionTransport(QObject* parent = nullptr);
  ~ConnectionTransport() override;

  bool addServer(const QString& serverId, const QString& serverUrl);
  bool hasServer(const QString& serverId) const;
  bool connectServer(const QString& serverId);
  bool removeServer(const QString& serverId);
  void removeAllServers();

  bool sendTextMessage(const QString& serverId, const QString& message);
  bool sendBinaryMessage(const QString& serverId, const QByteArray& data);
  void setReconnectBlocked(const QString& serverId, bool blocked);

  bool isConnected(const QString& serverId) const;
  QString httpsBaseUrl(const QString& serverId) const;
  ConnectionStatus status(const QString& serverId) const;
  QStringList connectedServers() const;
  QStringList allServers() const;

  void setReconnectInterval(int milliseconds);
  void setMaxReconnectAttempts(int attempts);
  void setConnectionTimeout(int milliseconds);
  void setPingInterval(int milliseconds);
  void setDeadConnectionTimeout(int milliseconds);
  void setSyncInProgress(const QString& serverId, bool inProgress);

 signals:
  void textMessageReceived(const QString& serverId, const QString& message);
  void binaryMessageReceived(const QString& serverId, const QByteArray& data);
  void connectionStatusChanged(const QString& serverId, int status);
  void socketErrorOccurred(const QString& serverId,
                           const QString& errorMessage);
  void sslErrorOccurred(const QString& serverId, const QString& errorMessage);
  void disconnected(const QString& serverId);
  void pongReceived(const QString& serverId, quint64 elapsedTime);

 private slots:
  void onConnected();
  void onDisconnected();
  void onTextMessageReceived(const QString& message);
  void onBinaryMessageReceived(const QByteArray& data);
  void onError(QAbstractSocket::SocketError error);
  void onSslErrors(const QList<QSslError>& errors);
  void onPong(quint64 elapsedTime, const QByteArray& payload);
  void checkConnections();
  void onConnectionTimeout();

 private:
  bool addServerImpl(const QString& serverId, const QString& serverUrl);
  bool hasServerImpl(const QString& serverId) const;
  bool connectServerImpl(const QString& serverId);
  bool removeServerImpl(const QString& serverId);
  void removeAllServersImpl();
  bool sendTextMessageImpl(const QString& serverId, const QString& message);
  bool sendBinaryMessageImpl(const QString& serverId, const QByteArray& data);
  void setReconnectBlockedImpl(const QString& serverId, bool blocked);
  bool isConnectedImpl(const QString& serverId) const;
  QString httpsBaseUrlImpl(const QString& serverId) const;
  ConnectionStatus statusImpl(const QString& serverId) const;
  QStringList connectedServersImpl() const;
  QStringList allServersImpl() const;
  void setReconnectIntervalImpl(int milliseconds);
  void setMaxReconnectAttemptsImpl(int attempts);
  void setConnectionTimeoutImpl(int milliseconds);
  void setPingIntervalImpl(int milliseconds);
  void setDeadConnectionTimeoutImpl(int milliseconds);
  void setSyncInProgressImpl(const QString& serverId, bool inProgress);
  void connectToServer(ServerConnection* conn);
  void scheduleReconnect(ServerConnection* conn);
  void updateConnectionStatus(ServerConnection* conn, ConnectionStatus status);
  void startConnectionTimeout(ServerConnection* conn);
  void cancelConnectionTimeout(ServerConnection* conn);
  void sendPing(ServerConnection* conn);
  ServerConnection* findConnectionBySocket(QWebSocket* socket) const;
  ServerConnection* findConnectionByTimer(QTimer* timer) const;

  QHash<QString, std::shared_ptr<ServerConnection>> connections_;
  mutable QRecursiveMutex mutex_;
  QTimer* reconnectTimer_;
  QTimer* pingTimer_;
  int reconnectInterval_;
  int maxReconnectAttempts_;
  int connectionTimeout_;
  int pingInterval_;
  int deadConnectionTimeout_;
};

#endif  // CONNECTIONTRANSPORT_H