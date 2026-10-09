#pragma once

#include <QObject>

#include "doubleratchet.h"
#include "nlohmann/json.hpp"

class MainSignals : public QObject {
  Q_OBJECT
 public:
  static MainSignals& instance() {
    static MainSignals _instance;
    return _instance;
  }

  Q_INVOKABLE void emitContactClicked(const QString& contactPubKey) {
    emit contactClicked(contactPubKey);
  }

  Q_INVOKABLE void emitMessageSended(const QString& message_id,
                                     const QString& serverId,
                                     const QString& contactPubKey,
                                     const QString& messageText,
                                     quint64 timestamp, quint64 replyTo = 0,
                                     const QString& replyPreview = QString(),
                                     const QString& replyKind = QString(),
                                     bool replyIsOwn = false) {
    qDebug() << "Emitting messageSended signal:" << message_id << serverId
             << contactPubKey << messageText << timestamp;
    emit messageSended(message_id, serverId, contactPubKey, messageText,
                       timestamp, replyTo, replyPreview, replyKind, replyIsOwn);
  }

  Q_INVOKABLE void emitMessageStatusDelivered(const QString& message_id,
                                              const QString& serverId,
                                              const QString& contactPubKey) {
    qDebug() << "Emitting messageDeliveredStatus signal:" << message_id
             << serverId << contactPubKey;
    emit messageStatusDelivered(message_id, serverId, contactPubKey);
  }

  Q_INVOKABLE void emitMessageStatusSended(const QString& message_id,
                                           const QString& serverId,
                                           const QString& contactPubKey) {
    qDebug() << "Emitting messageSendedStatus signal:" << message_id << serverId
             << contactPubKey;
    emit messageStatusSended(message_id, serverId, contactPubKey);
  }

  Q_INVOKABLE void emitLastOnlineChanged(const QString& serverId,
                                         const QString& contactPubKey,
                                         const QString& lastOnline) {
    // qDebug() << "Emitting lastOnlineChanged signal:" << contactPubKey <<
    // lastOnline;
    emit lastOnlineChanged(serverId, contactPubKey, lastOnline);
  }

  Q_INVOKABLE void emitTimerTicked() {
    // qDebug() << "Timer ticked signal emitted.";
    emit timerTicked();
  }

  Q_INVOKABLE void emitOnlineRequest(const QString& serverAddress,
                                     const QString& contactPubKey) {
    // qDebug() << "Emitting onlineRequest signal:" << serverAddress <<
    // contactPubKey;
    emit onlineRequest(serverAddress, contactPubKey);
  }

  Q_INVOKABLE void emitE2eTextMessageReceived(
      const QString& serverId, const QString& fromPubKey,
      const QString& messageText, quint64 messageId, quint64 timestamp,
      quint64 replyTo = 0, const QString& replyPreview = QString(),
      const QString& replyKind = QString(), bool replyIsOwn = false,
      int timerSeconds = -1) {
    qDebug() << "Emitting e2eTextMessageReceived signal:" << serverId
             << fromPubKey << messageText << messageId << timestamp;
    emit e2eTextMessageReceived(serverId, fromPubKey, messageText, messageId,
                                timestamp, replyTo, replyPreview, replyKind,
                                replyIsOwn, timerSeconds);
  }

  // A file/audio/album message was received and persisted; FileTransferManager
  // listens to auto-download it per the user's Auto-download settings.
  Q_INVOKABLE void emitMediaMessageReceived(const QString& serverId,
                                            const QString& fromPubKey,
                                            quint64 messageId, bool isAlbum) {
    emit mediaMessageReceived(serverId, fromPubKey, messageId, isAlbum);
  }

  Q_INVOKABLE void emitSyncEnded(const QString& serverId) {
    qDebug() << "Emitting syncEnded signal for server:" << serverId;
    emit syncEnded(serverId);
  }

  Q_INVOKABLE void emitSyncStarted(const QString& serverId) {
    qDebug() << "Emitting syncStarted signal for server:" << serverId;
    emit syncStarted(serverId);
  }

  Q_INVOKABLE void emitAllMessagesReaded(const QString& serverId,
                                         const QString& contactPubKey) {
    qDebug() << "Emitting allMessagesReaded signal for contact:"
             << contactPubKey << "on server:" << serverId;
    emit allMessagesReaded(serverId, contactPubKey);
  }

  Q_INVOKABLE void emitAllOwnMessagesReaded(const QString& serverId,
                                            const QString& contactPubKey) {
    qDebug() << "Emitting allOwnMessagesReaded signal for contact:"
             << contactPubKey << "on server:" << serverId;
    emit allOwnMessagesReaded(serverId, contactPubKey);
  }

  Q_INVOKABLE void emitAvatarDeleted() {
    qDebug() << "Emitting avatarDeleted signal";
    emit avatarDeleted();
  }

  Q_INVOKABLE void emitSettingSetBool(const QString& key, bool value) {
    qDebug() << "Emitting settingSetBool signal:" << key << value;
    emit settingSetBool(key, value);
  }

  Q_INVOKABLE void emitSettingSetText(const QString& key,
                                      const QString& value) {
    qDebug() << "Emitting settingSetText signal:" << key << value;
    emit settingSetText(key, value);
  }

  Q_INVOKABLE void emitSettingSetBlob(const QString& key,
                                      const QByteArray& value) {
    qDebug() << "Emitting settingSetBlob signal:" << key << "with blob of size"
             << value.size();
    emit settingSetBlob(key, value);
  }

  Q_INVOKABLE void emitMessageDelete(const quint64 message_id,
                                     const QString& serverId,
                                     const QString& contactPubKey) {
    qDebug() << "Emitting messageDelete signal:" << message_id << serverId
             << contactPubKey;
    emit messageDelete(message_id, serverId, contactPubKey);
  }

  Q_INVOKABLE void emitLocalMessageDelete(const quint64 message_id,
                                          const QString& serverId,
                                          const QString& contactPubKey) {
    qDebug() << "Emitting localMessageDelete signal:" << message_id << serverId
             << contactPubKey;
    emit localMessageDelete(message_id, serverId, contactPubKey);
  }

  Q_INVOKABLE void emitMessageDeleteReceived(const quint64 message_id,
                                             const QString& serverId,
                                             const QString& contactPubKey) {
    qDebug() << "Emitting messageDeleteReceived signal:" << message_id
             << serverId << contactPubKey;
    emit messageDeleteReceived(message_id, serverId, contactPubKey);
  }

  Q_INVOKABLE void emitContactRemove(const QString& serverId,
                                     const QString& contactPubKey) {
    qDebug() << "Emitting contactRemove signal for contact:" << contactPubKey
             << "on server:" << serverId;
    emit contactRemove(serverId, contactPubKey);
  }

  Q_INVOKABLE void emitSessionRemoveReceived(const QString& serverId,
                                             const QString& contactPubKey) {
    qDebug() << "Emitting sessionRemoveReceived signal for contact:"
             << contactPubKey << "on server:" << serverId;
    emit sessionRemoveReceived(serverId, contactPubKey);
  }

  Q_INVOKABLE void emitIdentityRevocationReceived(
      const QString& serverId, const QString& contactPubKey) {
    qWarning() << "Emitting identityRevocationReceived signal for contact:"
               << contactPubKey << "on server:" << serverId;
    emit identityRevocationReceived(serverId, contactPubKey);
  }

  Q_INVOKABLE void emitContactRemovedLocally(const QString& serverId,
                                             const QString& contactPubKey) {
    qDebug() << "Emitting contactRemovedLocally signal for contact:"
             << contactPubKey << "on server:" << serverId;
    emit contactRemovedLocally(serverId, contactPubKey);
  }

  Q_INVOKABLE void emitDoubleRatchetUpdate(
      const QString& serverId, const QString& pubKeyFingerprint,
      const nlohmann::json& doubleRatchetJson) {
    qDebug() << "Emitting doubleRatchetUpdate signal for contact:" << serverId
             << pubKeyFingerprint;
    emit doubleRatchetUpdate(serverId, pubKeyFingerprint, doubleRatchetJson);
  }

  Q_INVOKABLE void emitUpdateMessageDeliveredStatus(
      const QString& serverId, const QString& contactPubKey,
      const quint64 messageId) {
    qDebug() << "Emitting updateMessageDeliveredStatus signal for message ID:"
             << messageId << "contact:" << serverId << contactPubKey;
    emit updateMessageDeliveredStatus(serverId, contactPubKey, messageId);
  }

  Q_INVOKABLE void emitMessageReaded(const QString& serverId,
                                     const QString& fromPubKey,
                                     const quint64 messageId) {
    qDebug() << "Emitting messageReaded signal for message ID:" << messageId
             << "from contact:" << fromPubKey << "on server:" << serverId;
    emit messageReaded(serverId, fromPubKey, messageId);
  }

  Q_INVOKABLE void emitClearHistory(const QString& serverId,
                                    const QString& contactPubKey) {
    qDebug() << "Emitting clearHistory signal for contact:" << contactPubKey
             << "on server:" << serverId;
    emit clearHistory(serverId, contactPubKey);
  }

  Q_INVOKABLE void emitClearHistoryReceived(const QString& serverId,
                                            const QString& contactPubKey) {
    qDebug() << "Emitting clearHistoryReceived signal for contact:"
             << contactPubKey << "on server:" << serverId;
    emit clearHistoryReceived(serverId, contactPubKey);
  }

  Q_INVOKABLE void emitSetTimer(const QString& serverId,
                                const QString& contactPubKey, int seconds) {
    qDebug() << "Emitting setTimer signal for contact:" << contactPubKey
             << "on server:" << serverId << "with timer:" << seconds
             << "seconds";
    emit setTimer(serverId, contactPubKey, seconds);
  }

  Q_INVOKABLE void emitSetTimerReceived(const QString& serverId,
                                        const QString& contactPubKey,
                                        int seconds) {
    qDebug() << "Emitting setTimer signal for contact:" << contactPubKey
             << "on server:" << serverId << "with timer:" << seconds
             << "seconds";
    emit setTimerReceived(serverId, contactPubKey, seconds);
  }

  Q_INVOKABLE void emitAddInfoMessage(const QString& serverId,
                                      const QString& toPubKey,
                                      const QString& text, quint64 messageId) {
    qDebug() << "Emitting addInfoMessage signal for message ID:" << messageId
             << "with text:" << text;
    emit addInfoMessage(serverId, toPubKey, text, messageId);
  }

  Q_INVOKABLE void emitSecondTimerTicked() {
    // qDebug() << "Emitting secondTimerTicker signal.";
    emit secondTimerTicked();
  }

  Q_INVOKABLE void emitSessionFingerprintChanged(
      const QString& serverId, const QString& fromPubKey,
      const QString& pubKeyFingerprint) {
    qDebug() << "Emitting sessionFingerprintChanged signal from contact:"
             << serverId << fromPubKey
             << "with new fingerprint:" << pubKeyFingerprint;
    emit sessionFingerprintChanged(serverId, fromPubKey, pubKeyFingerprint);
  }

 signals:
  // List of all global application events
  void sessionFingerprintChanged(const QString& serverId,
                                 const QString& fromPubKey,
                                 const QString& pubKeyFingerprint);
  void secondTimerTicked();
  void addInfoMessage(const QString& serverId, const QString& toPubKey,
                      const QString& text, quint64 messageId);
  void setTimer(const QString& serverId, const QString& contactPubKey,
                int seconds);
  void setTimerReceived(const QString& serverId, const QString& contactPubKey,
                        int seconds);
  void clearHistory(const QString& serverId, const QString& contactPubKey);
  void clearHistoryReceived(const QString& serverId,
                            const QString& contactPubKey);
  void messageReaded(const QString& serverId, const QString& fromPubKey,
                     const quint64 messageId);
  void updateMessageDeliveredStatus(const QString& serverId,
                                    const QString& contactPubKey,
                                    quint64 messageId);
  void doubleRatchetUpdate(const QString& serverId,
                           const QString& pubKeyFingerprint,
                           const nlohmann::json& doubleRatchetJson);
  void sessionRemoveReceived(const QString& serverId,
                             const QString& contactPubKey);
  // A peer (or the server, on their behalf) announced that this identity key is
  // compromised and must no longer be trusted.
  void identityRevocationReceived(const QString& serverId,
                                  const QString& contactPubKey);
  void contactRemove(const QString& serverId, const QString& contactPubKey);
  void contactRemovedLocally(const QString& serverId,
                             const QString& contactPubKey);
  void contactRemovedIndex(int index);
  void contactClicked(const QString& contactPubKey);
  void messageSended(const QString& message_id, const QString& serverId,
                     const QString& contactPubKey, const QString& messageText,
                     quint64 timestamp, quint64 replyTo = 0,
                     const QString& replyPreview = QString(),
                     const QString& replyKind = QString(),
                     bool replyIsOwn = false);
  void messageStatusDelivered(const QString& message_id,
                              const QString& serverId,
                              const QString& contactPubKey);
  void messageStatusSended(const QString& message_id, const QString& serverId,
                           const QString& contactPubKey);
  void lastOnlineChanged(const QString& serverId, const QString& contactPubKey,
                         const QString& lastOnline);
  void timerTicked();
  void onlineRequest(const QString& serverAddress,
                     const QString& contactPubKey);
  void e2eTextMessageReceived(const QString& serverId,
                              const QString& fromPubKey,
                              const QString& messageText, quint64 messageId,
                              quint64 timestamp, quint64 replyTo = 0,
                              const QString& replyPreview = QString(),
                              const QString& replyKind = QString(),
                              bool replyIsOwn = false, int timerSeconds = -1);
  void mediaMessageReceived(const QString& serverId, const QString& fromPubKey,
                            quint64 messageId, bool isAlbum);
  void syncEnded(const QString& serverId);
  void syncStarted(const QString& serverId);
  void allMessagesReaded(const QString& serverId, const QString& contactPubKey);
  void allOwnMessagesReaded(const QString& serverId,
                            const QString& contactPubKey);
  void avatarDeleted();

  void settingSetBool(const QString& key, bool value);
  void settingSetText(const QString& key, const QString& value);
  void settingSetBlob(const QString& key, const QByteArray& value);

  void messageDelete(const quint64 message_id, const QString& serverId,
                     const QString& contactPubKey);
  void localMessageDelete(const quint64 message_id, const QString& serverId,
                          const QString& contactPubKey);
  void messageDeleteReceived(const quint64 message_id, const QString& serverId,
                             const QString& contactPubKey);

 private:
  explicit MainSignals(QObject* parent = nullptr) : QObject(parent) {}
  // Disable copying
  MainSignals(const MainSignals&) = delete;
  MainSignals& operator=(const MainSignals&) = delete;
};