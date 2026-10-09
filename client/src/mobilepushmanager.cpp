#include "mobilepushmanager.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QUrl>

#include "androidsystemui.h"
#include "connectionmanager.h"
#include "settingsmanager.h"

namespace {

constexpr auto kConfigPath = "/fcm/client-config";

QString summarizeToken(const QString& token) {
  if (token.isEmpty()) {
    return QStringLiteral("empty");
  }

  const QString prefix = token.left(12);
  return QStringLiteral("len=%1 prefix=%2").arg(token.size()).arg(prefix);
}

}  // namespace

MobilePushManager::MobilePushManager(ConnectionManager* connectionManager,
                                     SettingsManager* settingsManager,
                                     AndroidSystemUi* androidSystemUi,
                                     QObject* parent)
    : QObject(parent),
      m_connectionManager(connectionManager),
      m_settingsManager(settingsManager),
      m_androidSystemUi(androidSystemUi) {
  qDebug() << "[MobilePushManager] Created";
#ifdef Q_OS_ANDROID
  if (m_connectionManager != nullptr) {
    connect(m_connectionManager, &ConnectionManager::authChanged, this,
            &MobilePushManager::onAuthChanged);
    connect(m_connectionManager, &ConnectionManager::fcmTokenRegistered, this,
            &MobilePushManager::onFcmTokenRegistered);
    connect(m_connectionManager, &ConnectionManager::fcmNotUsed, this,
            &MobilePushManager::onFcmNotUsed);
  }
  if (m_androidSystemUi != nullptr) {
    connect(m_androidSystemUi, &AndroidSystemUi::fcmTokenReady, this,
            &MobilePushManager::onFcmTokenReady);
    connect(m_androidSystemUi, &AndroidSystemUi::fcmTokenError, this,
            &MobilePushManager::onFcmTokenError);
  }
  connect(&m_networkAccessManager, &QNetworkAccessManager::finished, this,
          &MobilePushManager::onClientConfigReplyFinished);
#else
  Q_UNUSED(m_connectionManager);
  Q_UNUSED(m_settingsManager);
  Q_UNUSED(m_androidSystemUi);
#endif
}

QString MobilePushManager::hashedServerKey(const QString& serverId) const {
  return QString::fromLatin1(
      QCryptographicHash::hash(serverId.toUtf8(), QCryptographicHash::Sha256)
          .toHex());
}

QString MobilePushManager::settingsKey(const QString& serverId,
                                       const QString& suffix) const {
  return QStringLiteral("fcm.%1.%2").arg(hashedServerKey(serverId), suffix);
}

MobilePushManager::ServerState& MobilePushManager::stateForServer(
    const QString& serverId) {
  auto it = m_serverStates.find(serverId);
  if (it != m_serverStates.end()) {
    return it.value();
  }

  ServerState state;
  loadState(serverId, &state);
  qDebug() << "[MobilePushManager] Loaded initial state for" << serverId
           << "token=" << summarizeToken(state.lastToken)
           << "configHashEmpty=" << state.configHash.isEmpty()
           << "tokenDirty=" << state.tokenDirty
           << "fcmDisabled=" << state.fcmDisabled;
  return m_serverStates.insert(serverId, state).value();
}

void MobilePushManager::loadState(const QString& serverId,
                                  ServerState* state) const {
  if (state == nullptr || m_settingsManager == nullptr) {
    return;
  }

  state->lastToken = m_settingsManager->getTextSetting(
      settingsKey(serverId, QStringLiteral("token")));
  state->configHash = m_settingsManager->getTextSetting(
      settingsKey(serverId, QStringLiteral("config_hash")));
  state->tokenDirty = m_settingsManager->getBoolSetting(
      settingsKey(serverId, QStringLiteral("dirty")), false);
}

void MobilePushManager::persistState(const QString& serverId,
                                     const ServerState& state) {
  if (m_settingsManager == nullptr) {
    return;
  }

  m_settingsManager->setTextSetting(
      settingsKey(serverId, QStringLiteral("token")), state.lastToken);
  m_settingsManager->setTextSetting(
      settingsKey(serverId, QStringLiteral("config_hash")), state.configHash);
  m_settingsManager->setBoolSetting(
      settingsKey(serverId, QStringLiteral("dirty")), state.tokenDirty);
  qDebug() << "[MobilePushManager] Persisted state for" << serverId
           << "token=" << summarizeToken(state.lastToken)
           << "configHashEmpty=" << state.configHash.isEmpty()
           << "tokenDirty=" << state.tokenDirty
           << "fcmDisabled=" << state.fcmDisabled
           << "authSessionNeedsRegistration="
           << state.authSessionNeedsRegistration
           << "inFlightRequestId=" << state.inFlightRequestId;
}

void MobilePushManager::onAuthChanged(const QString& serverId, bool success) {
#ifndef Q_OS_ANDROID
  Q_UNUSED(serverId);
  Q_UNUSED(success);
  return;
#else
  if (serverId.isEmpty()) {
    return;
  }

  ServerState& state = stateForServer(serverId);
  qDebug() << "[MobilePushManager] authChanged for" << serverId
           << "success=" << success
           << "token=" << summarizeToken(state.lastToken)
           << "tokenDirty=" << state.tokenDirty
           << "inFlightRequestId=" << state.inFlightRequestId;
  if (!success) {
    state.authSessionNeedsRegistration = false;
    state.inFlightRequestId = 0;
    state.fcmDisabled = false;
    return;
  }

  state.fcmDisabled = false;
  state.authSessionNeedsRegistration = true;
  fetchClientConfig(serverId);
#endif
}

void MobilePushManager::applySslPolicy(QNetworkReply* reply) const {
#ifndef Q_OS_ANDROID
  Q_UNUSED(reply);
#else
  if (reply == nullptr) {
    return;
  }

  if (m_settingsManager == nullptr ||
      !m_settingsManager->getBoolSetting(QStringLiteral("ignoreSslErrors"),
                                         false)) {
    return;
  }

  connect(reply, &QNetworkReply::sslErrors, reply,
          [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
#endif
}

void MobilePushManager::applySslPolicy(QNetworkRequest& request) const {
#ifndef Q_OS_ANDROID
  Q_UNUSED(request);
#else
  const bool ignoreSslErrors =
      m_settingsManager != nullptr &&
      m_settingsManager->getBoolSetting(QStringLiteral("ignoreSslErrors"),
                                        false);
  qWarning() << "[MobilePushManager] [applySslPolicy] ignoreSslErrors setting="
             << ignoreSslErrors << "url=" << request.url();
  if (!ignoreSslErrors) {
    return;
  }

  QSslConfiguration sslConfig = request.sslConfiguration();
  sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
  request.setSslConfiguration(sslConfig);
  qWarning() << "[MobilePushManager] [applySslPolicy] VerifyNone applied"
             << "verifyMode=" << sslConfig.peerVerifyMode();
#endif
}

void MobilePushManager::fetchClientConfig(const QString& serverId) {
#ifndef Q_OS_ANDROID
  Q_UNUSED(serverId);
#else
  if (m_connectionManager == nullptr) {
    return;
  }

  const QString httpsBaseUrl = m_connectionManager->getHttpsBaseUrl(serverId);
  const QString accessToken = m_connectionManager->getAccessToken(serverId);
  if (httpsBaseUrl.isEmpty()) {
    qWarning() << "[MobilePushManager] Missing HTTPS base URL for" << serverId;
    registerTokenIfNeeded(serverId);
    return;
  }
  if (accessToken.isEmpty()) {
    qWarning() << "[MobilePushManager] Missing access token for FCM client "
                  "config request on"
               << serverId;
    return;
  }

  const QUrl url(httpsBaseUrl + QString::fromLatin1(kConfigPath));
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
  applySslPolicy(request);
  qDebug() << "[MobilePushManager] Requesting FCM client config"
           << "serverId=" << serverId << "url=" << url
           << "hasAccessToken=" << !accessToken.isEmpty();
  QNetworkReply* reply = m_networkAccessManager.get(request);
  reply->setProperty("serverId", serverId);
  applySslPolicy(reply);
#endif
}

void MobilePushManager::disableFcmForServer(const QString& serverId,
                                            const QString& reason) {
#ifndef Q_OS_ANDROID
  Q_UNUSED(serverId);
  Q_UNUSED(reason);
#else
  if (serverId.isEmpty()) {
    return;
  }

  qDebug() << "[MobilePushManager] Disabling FCM for" << serverId
           << "reason=" << reason;
  ServerState& state = stateForServer(serverId);
  state.lastToken.clear();
  state.configHash.clear();
  state.tokenDirty = false;
  state.fcmDisabled = true;
  state.authSessionNeedsRegistration = false;
  state.inFlightRequestId = 0;
  persistState(serverId, state);

  if (m_androidSystemUi != nullptr) {
    m_androidSystemUi->clearFcmServerState(serverId);
  }
#endif
}

void MobilePushManager::onClientConfigReplyFinished(QNetworkReply* reply) {
#ifndef Q_OS_ANDROID
  Q_UNUSED(reply);
#else
  if (reply == nullptr) {
    return;
  }

  const QString serverId = reply->property("serverId").toString();
  const QByteArray responseBody = reply->readAll();
  const bool requestFailed = reply->error() != QNetworkReply::NoError;
  const QString errorText = reply->errorString();
  const int statusCode =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  reply->deleteLater();

  if (serverId.isEmpty()) {
    return;
  }

  if (!requestFailed && statusCode == 204) {
    disableFcmForServer(serverId, QStringLiteral("server disabled FCM"));
    return;
  }

  if (requestFailed) {
    qWarning() << "[MobilePushManager] Failed to fetch FCM client config"
               << "serverId=" << serverId << "statusCode=" << statusCode
               << "error=" << errorText;
    registerTokenIfNeeded(serverId);
    return;
  }

  qDebug() << "[MobilePushManager] FCM client config reply received"
           << "serverId=" << serverId << "statusCode=" << statusCode
           << "bytes=" << responseBody.size();

  ServerState& state = stateForServer(serverId);
  const QString configJson = QString::fromUtf8(responseBody);
  const QString configHash = QString::fromLatin1(
      QCryptographicHash::hash(responseBody, QCryptographicHash::Sha256)
          .toHex());
  const bool configChanged = state.configHash != configHash;
  qDebug() << "[MobilePushManager] Processed FCM config"
           << "serverId=" << serverId << "configChanged=" << configChanged
           << "configHash=" << configHash.left(12);
  state.configHash = configHash;
  state.fcmDisabled = false;
  if (configChanged) {
    state.tokenDirty = true;
    persistState(serverId, state);
  }

  if (m_androidSystemUi != nullptr) {
    qDebug() << "[MobilePushManager] Initializing Android FCM runtime for"
             << serverId;
    m_androidSystemUi->initializeFcmForServer(serverId, configJson);
    const QString cachedToken =
        m_androidSystemUi->getCachedFcmTokenForServer(serverId);
    qDebug() << "[MobilePushManager] Android cached token check"
             << "serverId=" << serverId
             << "token=" << summarizeToken(cachedToken);
    if (!cachedToken.isEmpty()) {
      onFcmTokenReady(serverId, cachedToken);
    }
    if (configChanged || cachedToken.isEmpty() || state.tokenDirty) {
      qDebug() << "[MobilePushManager] Requesting Android FCM token"
               << "serverId=" << serverId << "configChanged=" << configChanged
               << "cachedTokenEmpty=" << cachedToken.isEmpty()
               << "tokenDirty=" << state.tokenDirty;
      m_androidSystemUi->requestFcmTokenForServer(serverId);
    }
  }
#endif
}

void MobilePushManager::onFcmTokenReady(const QString& serverId,
                                        const QString& token) {
#ifndef Q_OS_ANDROID
  Q_UNUSED(serverId);
  Q_UNUSED(token);
#else
  if (serverId.isEmpty() || token.isEmpty()) {
    return;
  }

  ServerState& state = stateForServer(serverId);
  if (state.fcmDisabled) {
    qDebug()
        << "[MobilePushManager] Ignoring FCM token because FCM is disabled for"
        << serverId;
    return;
  }

  qDebug() << "[MobilePushManager] FCM token ready" << "serverId=" << serverId
           << "token=" << summarizeToken(token);
  if (state.lastToken != token) {
    state.lastToken = token;
    state.tokenDirty = true;
    qDebug() << "[MobilePushManager] FCM token changed for" << serverId;
  }
  persistState(serverId, state);
  registerTokenIfNeeded(serverId);
#endif
}

void MobilePushManager::onFcmTokenError(const QString& serverId,
                                        const QString& errorText) {
#ifndef Q_OS_ANDROID
  Q_UNUSED(serverId);
  Q_UNUSED(errorText);
#else
  qWarning() << "[MobilePushManager] Failed to obtain FCM token for" << serverId
             << ':' << errorText;
#endif
}

void MobilePushManager::registerTokenIfNeeded(const QString& serverId) {
#ifndef Q_OS_ANDROID
  Q_UNUSED(serverId);
#else
  if (m_connectionManager == nullptr ||
      !m_connectionManager->isAuthenticated(serverId)) {
    qDebug() << "[MobilePushManager] Skipping token registration because "
                "server is not authenticated:"
             << serverId;
    return;
  }

  ServerState& state = stateForServer(serverId);
  if (state.fcmDisabled) {
    qDebug() << "[MobilePushManager] Skipping token registration because FCM "
                "is disabled for"
             << serverId;
    return;
  }
  if (state.lastToken.isEmpty() || state.inFlightRequestId != 0) {
    qDebug() << "[MobilePushManager] Skipping token registration"
             << "serverId=" << serverId
             << "token=" << summarizeToken(state.lastToken)
             << "inFlightRequestId=" << state.inFlightRequestId;
    return;
  }
  if (!state.authSessionNeedsRegistration && !state.tokenDirty) {
    qDebug() << "[MobilePushManager] Token registration not needed for"
             << serverId;
    return;
  }

  qDebug() << "[MobilePushManager] Sending FCM token registration"
           << "serverId=" << serverId
           << "token=" << summarizeToken(state.lastToken)
           << "authSessionNeedsRegistration="
           << state.authSessionNeedsRegistration
           << "tokenDirty=" << state.tokenDirty;
  const uint32_t requestId =
      m_connectionManager->registerFcmToken(serverId, state.lastToken);
  if (requestId == 0) {
    qWarning() << "[MobilePushManager] Failed to send FCM token registration "
                  "request for"
               << serverId;
    return;
  }

  state.inFlightRequestId = requestId;
  persistState(serverId, state);
  qDebug() << "[MobilePushManager] Sent FCM token registration for" << serverId
           << "requestId=" << requestId;
#endif
}

void MobilePushManager::onFcmTokenRegistered(const QString& serverId,
                                             uint32_t requestId) {
#ifndef Q_OS_ANDROID
  Q_UNUSED(serverId);
  Q_UNUSED(requestId);
#else
  ServerState& state = stateForServer(serverId);
  if (state.inFlightRequestId != requestId) {
    qWarning() << "[MobilePushManager] Received unexpected FCM registration ack"
               << "serverId=" << serverId << "requestId=" << requestId
               << "expectedRequestId=" << state.inFlightRequestId;
    return;
  }

  state.inFlightRequestId = 0;
  state.tokenDirty = false;
  state.fcmDisabled = false;
  state.authSessionNeedsRegistration = false;
  persistState(serverId, state);
  qDebug() << "[MobilePushManager] FCM token registration acknowledged for"
           << serverId;
#endif
}

void MobilePushManager::onFcmNotUsed(const QString& serverId,
                                     uint32_t requestId) {
#ifndef Q_OS_ANDROID
  Q_UNUSED(serverId);
  Q_UNUSED(requestId);
#else
  ServerState& state = stateForServer(serverId);
  if (state.inFlightRequestId != 0 && state.inFlightRequestId != requestId) {
    qWarning() << "[MobilePushManager] Received unexpected FCM disabled ack"
               << "serverId=" << serverId << "requestId=" << requestId
               << "expectedRequestId=" << state.inFlightRequestId;
  }

  disableFcmForServer(serverId, QStringLiteral("server replied FCM not used"));
#endif
}