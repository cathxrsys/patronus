#include "connectionstorage.h"

#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include "databasemanager.h"
#include "messagemodel.h"

ConnectionStorage::ConnectionStorage(DatabaseManager* db) : m_db(db) {}

QStringList ConnectionStorage::loadKnownServerAddresses() const {
  if (m_db == nullptr) {
    return {};
  }

  QSet<QString> addresses;

  const QVariantList contactRows =
      m_db->select("SELECT serverAddress FROM contacts", {});
  for (const QVariant& rowVar : contactRows) {
    const QString address = rowVar.toMap().value("serverAddress").toString();
    if (!address.isEmpty()) {
      addresses.insert(address);
    }
  }

  const QVariantList settingRows = m_db->select(
      "SELECT value FROM textsettings WHERE name = ?", {"serverAddress"});
  for (const QVariant& rowVar : settingRows) {
    const QString address = rowVar.toMap().value("value").toString();
    if (!address.isEmpty()) {
      addresses.insert(address);
    }
  }

  return addresses.values();
}

bool ConnectionStorage::hasPrekey(const QString& prekeyId) const {
  return m_db != nullptr && m_db->prekeyExists(prekeyId);
}

bool ConnectionStorage::hasPrekey(const std::vector<uint8_t>& prekeyId) const {
  return m_db != nullptr && m_db->prekeyExists(prekeyId);
}

pqdh::KeyPairs ConnectionStorage::loadPrekey(const QString& prekeyId) const {
  return m_db != nullptr ? m_db->getPrekey(prekeyId) : pqdh::KeyPairs{};
}

pqdh::KeyPairs ConnectionStorage::loadPrekey(
    const std::vector<uint8_t>& prekeyId) const {
  return m_db != nullptr ? m_db->getPrekey(prekeyId) : pqdh::KeyPairs{};
}

void ConnectionStorage::storeAnonymousContactSession(
    const QString& serverId, const QString& contactPubKey,
    const dr::DoubleRatchet& doubleRatchet) const {
  if (m_db == nullptr) {
    return;
  }

  const QString serializedDoubleRatchet =
      QString::fromStdString(doubleRatchet.to_json().dump());
  m_db->execute(
      "INSERT INTO contacts (pubKey, firstName, lastName, aboutMe, avatar, "
      "serverAddress, doubleratchet) VALUES (?, '', '', '', '', ?, ?)",
      {contactPubKey, serverId, serializedDoubleRatchet});
}

void ConnectionStorage::storePendingContactSession(
    const QString& serverId, const QString& contactPubKey,
    const dr::DoubleRatchet& doubleRatchet) const {
  if (m_db == nullptr) {
    return;
  }

  const QString serializedDoubleRatchet =
      QString::fromStdString(doubleRatchet.to_json().dump());

  const QVariantList rows = m_db->select(
      "SELECT isPending FROM contacts WHERE serverAddress = ? AND pubKey = ?",
      {serverId, contactPubKey});
  if (rows.isEmpty()) {
    m_db->execute(
        "INSERT INTO contacts (pubKey, firstName, lastName, aboutMe, avatar, "
        "serverAddress, doubleratchet, isPending) VALUES (?, '', '', '', '', "
        "?, ?, 1)",
        {contactPubKey, serverId, serializedDoubleRatchet});
    return;
  }

  // Materialized contact: keep the established session (same semantics as the
  // old plain-INSERT path, which failed silently on the PK conflict). Only a
  // still-pending row is refreshed, so a legit initiator can retry.
  if (rows.first().toMap().value("isPending").toInt() == 1) {
    m_db->execute(
        "UPDATE contacts SET doubleratchet = ? WHERE serverAddress = ? AND "
        "pubKey = ? AND isPending = 1",
        {serializedDoubleRatchet, serverId, contactPubKey});
  }
}

void ConnectionStorage::materializePendingContact(
    const QString& serverId, const QString& contactPubKey) const {
  if (m_db == nullptr) {
    return;
  }

  m_db->execute(
      "UPDATE contacts SET isPending = 0 WHERE serverAddress = ? AND pubKey = "
      "?",
      {serverId, contactPubKey});
}

QStringList ConnectionStorage::loadUnenrolledContactPubKeys(
    const QString& serverId) const {
  if (m_db == nullptr) {
    return {};
  }

  QStringList pubKeys;
  const QVariantList rows = m_db->select(
      "SELECT pubKey FROM contacts WHERE serverAddress = ? AND isPending = 0 "
      "AND memberEnrolled = 0",
      {serverId});
  pubKeys.reserve(rows.size());
  for (const QVariant& rowVar : rows) {
    const QString pubKey = rowVar.toMap().value("pubKey").toString();
    if (!pubKey.isEmpty()) {
      pubKeys.append(pubKey);
    }
  }
  return pubKeys;
}

void ConnectionStorage::markContactEnrolled(
    const QString& serverId, const QString& contactPubKey) const {
  if (m_db == nullptr) {
    return;
  }

  m_db->execute(
      "UPDATE contacts SET memberEnrolled = 1 WHERE serverAddress = ? AND "
      "pubKey = ?",
      {serverId, contactPubKey});
}

dr::DoubleRatchet ConnectionStorage::loadDoubleRatchet(
    const QString& serverId, const QString& contactPubKey) const {
  return m_db != nullptr ? m_db->getDoubleRatchet(serverId, contactPubKey)
                         : dr::DoubleRatchet{};
}

QList<PendingTextMessage> ConnectionStorage::loadPendingTextMessages(
    const QString& serverId) const {
  QList<PendingTextMessage> pendingMessages;
  if (m_db == nullptr) {
    return pendingMessages;
  }

  const QVariantList rows = m_db->select(
      QStringLiteral("SELECT id, fromPubKey, text, t FROM messages WHERE "
                     "serverAddress = ? AND isOwn = 1 AND isSended = 0 AND "
                     "type = ? ORDER BY t ASC, id ASC"),
      {serverId, static_cast<int>(MessageModel::MESSAGE_TYPE::TEXT)});

  pendingMessages.reserve(rows.size());
  for (const QVariant& rowVar : rows) {
    const QVariantMap row = rowVar.toMap();
    PendingTextMessage message;
    message.id = row.value(QStringLiteral("id")).toULongLong();
    message.contactPubKey = row.value(QStringLiteral("fromPubKey")).toString();
    message.text = row.value(QStringLiteral("text")).toString();
    message.timestamp = row.value(QStringLiteral("t")).toULongLong();
    if (message.id == 0 || message.contactPubKey.isEmpty() ||
        message.text.isEmpty()) {
      continue;
    }
    pendingMessages.append(message);
  }

  return pendingMessages;
}

void ConnectionStorage::saveDoubleRatchet(
    const QString& serverId, const QString& contactPubKey,
    const dr::DoubleRatchet& doubleRatchet) const {
  if (m_db == nullptr) {
    return;
  }

  m_db->setDoubleRatchet(serverId, contactPubKey, doubleRatchet);
}

void ConnectionStorage::addTypedMessage(
    const quint64 messageId, const QString& serverId,
    const QString& contactPubKey, const QString& text, int messageType,
    quint64 timestamp, bool isOwn, bool isSended, bool isReaded,
    bool isReceived, const QByteArray& content, const ReplyInfo& reply) const {
  if (m_db == nullptr) {
    return;
  }

  m_db->addTypedMessage(messageId, serverId, contactPubKey, text,
                        static_cast<MessageModel::MESSAGE_TYPE>(messageType),
                        timestamp, isOwn, isSended, isReaded, isReceived,
                        content, reply);
}

void ConnectionStorage::addCallMessageAsync(
    const quint64 messageId, const QString& serverId,
    const QString& contactPubKey, const QString& text, quint64 timestamp,
    bool isOwn, bool isSended, bool isReaded, bool isReceived) const {
  if (m_db == nullptr) {
    return;
  }

  m_db->NEW_THREAD_addMessage(messageId, serverId, contactPubKey, text,
                              timestamp, isOwn, true, isSended, isReaded,
                              isReceived, true);
}

void ConnectionStorage::updateMessageTimestamp(const QString& serverId,
                                               const QString& contactPubKey,
                                               quint64 messageId,
                                               quint64 timestamp) const {
  if (m_db == nullptr || timestamp == 0) {
    return;
  }

  m_db->execute(QStringLiteral("UPDATE messages SET t = ? WHERE serverAddress "
                               "= ? AND fromPubKey = ? AND id = ?"),
                {timestamp, serverId, contactPubKey, messageId});
}

void ConnectionStorage::updateMessageSentStatus(const QString& serverId,
                                                const QString& contactPubKey,
                                                quint64 messageId) const {
  if (m_db == nullptr) {
    return;
  }

  m_db->updateMessageSentStatus(serverId, contactPubKey, messageId);
}

void ConnectionStorage::updateMessageDeliveredStatus(
    const QString& serverId, const QString& contactPubKey,
    quint64 messageId) const {
  if (m_db == nullptr) {
    return;
  }

  m_db->updateMessageDeliveredStatus(serverId, contactPubKey, messageId);
}