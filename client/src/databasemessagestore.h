#pragma once

#include <QByteArray>
#include <QSqlDatabase>

#include "messagemodel.h"

class DatabaseMessageStore {
 public:
  explicit DatabaseMessageStore(QSqlDatabase* database = nullptr);

  void setDatabase(QSqlDatabase* database);

  quint64 insertTypedMessageValue(
      quint64 mid, const QString& serverId, const QString& toPubKey,
      const QString& text, MessageModel::MESSAGE_TYPE type, quint64 timestamp,
      bool isOwn, bool isSended, bool isReaded, bool isReceived,
      const QByteArray& content = QByteArray(),
      const ReplyInfo& reply = {}) const;
  void addMessage(quint64 mid, const QString& serverId, const QString& toPubKey,
                  const QString& text, quint64 timestamp, bool isOwn,
                  bool isCall, bool isSended, bool isReaded, bool isReceived,
                  int timerValue, const ReplyInfo& reply = {}) const;
  void markMessagesAsReaded(const QString& serverId,
                            const QString& contactPubKey) const;
  void markOwnMessagesAsReaded(const QString& serverId,
                               const QString& contactPubKey) const;
  void updateMessageSendedStatus(const QString& serverId,
                                 const QString& contactPubKey,
                                 quint64 id) const;
  void updateMessageDeliveredStatus(const QString& serverId,
                                    const QString& contactPubKey,
                                    quint64 id) const;
  void updateCallDuration(const QString& serverId,
                          const QString& contactPubKey, quint64 id,
                          quint64 durationSec) const;
  void deleteMessage(const QString& serverId, const QString& contactPubKey,
                     quint64 id) const;
  void deleteAllMessagesFromContact(const QString& serverId,
                                    const QString& contactPubKey) const;
  QByteArray loadMessageContentValue(const QString& serverId,
                                     const QString& contactPubKey,
                                     quint64 messageId) const;
  bool saveMessageContentValue(const QString& serverId,
                               const QString& contactPubKey, quint64 id,
                               const QByteArray& content) const;
  int unreadCount(const QString& serverId, const QString& contactPubKey) const;
  Message loadLastMessage(const QString& serverId,
                          const QString& contactPubKey) const;
  QList<Message> loadMessages(const QString& serverId,
                              const QString& contactPubKey) const;

 private:
  bool ensureOpen() const;

  QSqlDatabase* m_database = nullptr;
};