#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "doubleratchet.h"
#include "pqdh.h"

class DatabaseManager;
struct ReplyInfo;

struct PendingTextMessage {
  quint64 id = 0;
  QString contactPubKey;
  QString text;
  quint64 timestamp = 0;
};

class ConnectionStorage {
 public:
  explicit ConnectionStorage(DatabaseManager* db = nullptr);

  QStringList loadKnownServerAddresses() const;

  bool hasPrekey(const QString& prekeyId) const;
  bool hasPrekey(const std::vector<uint8_t>& prekeyId) const;
  pqdh::KeyPairs loadPrekey(const QString& prekeyId) const;
  pqdh::KeyPairs loadPrekey(const std::vector<uint8_t>& prekeyId) const;

  void storeAnonymousContactSession(
      const QString& serverId, const QString& contactPubKey,
      const dr::DoubleRatchet& doubleRatchet) const;

  // Stores an invite-gated session created by an incoming session_request as a
  // PENDING contact row (invisible to the contacts list) until the peer proves
  // knowledge of our invite secret with a first successfully decrypted
  // message. An existing pending row is replaced (the initiator may retry with
  // a fresh session); an existing materialized contact is left untouched.
  void storePendingContactSession(const QString& serverId,
                                  const QString& contactPubKey,
                                  const dr::DoubleRatchet& doubleRatchet) const;
  // Promotes a pending row to a real contact after the first successful
  // decrypt. No-op for rows that are already materialized.
  void materializePendingContact(const QString& serverId,
                                 const QString& contactPubKey) const;

  // Materialized (non-pending) contacts the server has not yet confirmed as
  // enrolled (see ResponseEnrollAck) - candidates for
  // ConnectionManager::retryPendingMemberEnrollments() on reconnect.
  QStringList loadUnenrolledContactPubKeys(const QString& serverId) const;
  void markContactEnrolled(const QString& serverId,
                           const QString& contactPubKey) const;

  dr::DoubleRatchet loadDoubleRatchet(const QString& serverId,
                                      const QString& contactPubKey) const;
  void saveDoubleRatchet(const QString& serverId, const QString& contactPubKey,
                         const dr::DoubleRatchet& doubleRatchet) const;
  QList<PendingTextMessage> loadPendingTextMessages(
      const QString& serverId) const;

  void addTypedMessage(const quint64 messageId, const QString& serverId,
                       const QString& contactPubKey, const QString& text,
                       int messageType, quint64 timestamp, bool isOwn,
                       bool isSended, bool isReaded, bool isReceived,
                       const QByteArray& content, const ReplyInfo& reply) const;

  void addCallMessageAsync(const quint64 messageId, const QString& serverId,
                           const QString& contactPubKey, const QString& text,
                           quint64 timestamp, bool isOwn, bool isSended,
                           bool isReaded, bool isReceived) const;
  void updateMessageTimestamp(const QString& serverId,
                              const QString& contactPubKey, quint64 messageId,
                              quint64 timestamp) const;
  void updateMessageSentStatus(const QString& serverId,
                               const QString& contactPubKey,
                               quint64 messageId) const;
  void updateMessageDeliveredStatus(const QString& serverId,
                                    const QString& contactPubKey,
                                    quint64 messageId) const;

 private:
  DatabaseManager* m_db = nullptr;
};