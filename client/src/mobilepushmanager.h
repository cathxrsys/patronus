#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>

class AndroidSystemUi;
class ConnectionManager;
class QNetworkReply;
class QNetworkRequest;
class SettingsManager;

class MobilePushManager : public QObject {
  Q_OBJECT

 public:
  explicit MobilePushManager(ConnectionManager* connectionManager,
                             SettingsManager* settingsManager,
                             AndroidSystemUi* androidSystemUi,
                             QObject* parent = nullptr);

 private slots:
  void onAuthChanged(const QString& serverId, bool success);
  void onFcmTokenReady(const QString& serverId, const QString& token);
  void onFcmTokenError(const QString& serverId, const QString& errorText);
  void onFcmTokenRegistered(const QString& serverId, uint32_t requestId);
  void onFcmNotUsed(const QString& serverId, uint32_t requestId);
  void onClientConfigReplyFinished(QNetworkReply* reply);

 private:
  struct ServerState {
    QString lastToken;
    QString configHash;
    bool tokenDirty = false;
    bool fcmDisabled = false;
    bool authSessionNeedsRegistration = false;
    uint32_t inFlightRequestId = 0;
  };

  QString hashedServerKey(const QString& serverId) const;
  QString settingsKey(const QString& serverId, const QString& suffix) const;
  ServerState& stateForServer(const QString& serverId);
  void applySslPolicy(QNetworkReply* reply) const;
  void applySslPolicy(QNetworkRequest& request) const;
  void disableFcmForServer(const QString& serverId, const QString& reason);
  void fetchClientConfig(const QString& serverId);
  void registerTokenIfNeeded(const QString& serverId);
  void persistState(const QString& serverId, const ServerState& state);
  void loadState(const QString& serverId, ServerState* state) const;

  ConnectionManager* m_connectionManager = nullptr;
  SettingsManager* m_settingsManager = nullptr;
  AndroidSystemUi* m_androidSystemUi = nullptr;
  QNetworkAccessManager m_networkAccessManager;
  QHash<QString, ServerState> m_serverStates;
};