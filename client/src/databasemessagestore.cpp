#include "databasemessagestore.h"

#include <QDateTime>
#include <QDebug>
#include <QLocale>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>

#include "apppaths.h"
#include "filemessageutils.h"
#include "localeutils.h"
#include "voicecliputils.h"

namespace {

void restoreAudioWaveformIfNeeded(FileMessageData* fileData) {
  if (fileData == nullptr || !filemessage::isAudioMessage(*fileData) ||
      !fileData->waveform.isEmpty() || fileData->localPath.isEmpty()) {
    return;
  }

  fileData->waveform = voiceclip::buildWaveformFromFile(fileData->localPath);
}

QLocale resolveMessageLocale() {
  QSettings settings(apppaths::settingsFilePath(), QSettings::IniFormat);
  return localeutils::qLocaleForUiLanguage(
      settings.value("ui/language", "en").toString());
}

Message mapMessageRow(const QSqlQuery& query) {
  Message message;
  const MessageModel::MESSAGE_TYPE msgType =
      static_cast<MessageModel::MESSAGE_TYPE>(query.value("type").toUInt());

  if (msgType == MessageModel::MESSAGE_TYPE::TEXT) {
    message.text = query.value("text").toString();
    message.isMsg = true;
  } else if (msgType == MessageModel::MESSAGE_TYPE::CALL) {
    message.text = query.value("text").toString();
    message.isMsg = true;
    message.isCall = true;
    message.callDurationSec = query.value("callDurationSec").toULongLong();
  } else if (msgType == MessageModel::MESSAGE_TYPE::INFO) {
    message.text = query.value("text").toString();
    message.isInfo = true;
  } else if (msgType == MessageModel::MESSAGE_TYPE::FILE ||
             msgType == MessageModel::MESSAGE_TYPE::AUDIO) {
    message.text = query.value("text").toString();
    message.isMsg = true;
    message.isFile = true;
    message.isAudio = (msgType == MessageModel::MESSAGE_TYPE::AUDIO);
    filemessage::deserialize(query.value("content").toByteArray(),
                             &message.fileData);
    restoreAudioWaveformIfNeeded(&message.fileData);
  } else if (msgType == MessageModel::MESSAGE_TYPE::ALBUM) {
    message.text = query.value("text").toString();  // caption
    message.isMsg = true;
    message.isAlbum = true;
    filemessage::deserializeAlbum(query.value("content").toByteArray(),
                                  &message.albumData);
    message.albumItemDownloading =
        QList<bool>(message.albumData.items.size(), false);
    message.albumItemProgress =
        QList<qreal>(message.albumData.items.size(), 0.0);
  }

  const quint64 timestamp = query.value("t").toULongLong();
  const QDateTime dt = QDateTime::fromSecsSinceEpoch(timestamp);
  message.time = dt.toString("HH:mm");
  message.t = timestamp;
  message.isOwn = query.value("isOwn").toBool();
  message.isSended = query.value("isSended").toBool();
  message.isReaded = query.value("isReaded").toBool();
  message.isReceived = query.value("isReceived").toBool();
  message.id = query.value("id").toULongLong();
  message.mid = message.id;
  message.replyTo = query.value("replyTo").toULongLong();
  message.replyPreview = query.value("replyPreview").toString();
  message.replyKind = query.value("replyKind").toString();
  message.replyIsOwn = query.value("replyIsOwn").toBool();
  return message;
}

}  // namespace

DatabaseMessageStore::DatabaseMessageStore(QSqlDatabase* database)
    : m_database(database) {}

void DatabaseMessageStore::setDatabase(QSqlDatabase* database) {
  m_database = database;
}

bool DatabaseMessageStore::ensureOpen() const {
  if (m_database == nullptr) {
    qCritical() << "MessageStore: database is null";
    return false;
  }

  if (m_database->isOpen()) {
    return true;
  }

  if (!m_database->open()) {
    qCritical() << "MessageStore: DB not open and cannot reopen!";
    return false;
  }

  return true;
}

quint64 DatabaseMessageStore::insertTypedMessageValue(
    quint64 mid, const QString& serverId, const QString& toPubKey,
    const QString& text, MessageModel::MESSAGE_TYPE type, quint64 timestamp,
    bool isOwn, bool isSended, bool isReaded, bool isReceived,
    const QByteArray& content, const ReplyInfo& reply) const {
  if (!ensureOpen()) {
    return static_cast<quint64>(-1);
  }

  QSqlQuery query(*m_database);
  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(
                          QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
  query.prepare(
      "INSERT OR REPLACE INTO messages (id, serverAddress, fromPubKey, type, "
      "text, t, isOwn, isSended, isReaded, isReceived, content, replyTo, "
      "replyPreview, replyKind, replyIsOwn) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, "
      "?, ?, ?, ?, ?, ?)");
  query.bindValue(0, mid);
  query.bindValue(1, serverId);
  query.bindValue(2, toPubKey);
  query.bindValue(3, static_cast<uint8_t>(type));
  query.bindValue(4, text);
  query.bindValue(5, effectiveTimestamp);
  query.bindValue(6, isOwn ? 1 : 0);
  query.bindValue(7, isSended ? 1 : 0);
  query.bindValue(8, isReaded ? 1 : 0);
  query.bindValue(9, isReceived ? 1 : 0);
  query.bindValue(10, content);
  // See addMessage: coerce null reply strings to empty so the NOT NULL
  // replyPreview / replyKind columns never receive SQL NULL.
  query.bindValue(11, static_cast<qulonglong>(reply.replyTo));
  query.bindValue(12, reply.preview.isNull() ? QString("") : reply.preview);
  query.bindValue(13, reply.kind.isNull() ? QString("") : reply.kind);
  query.bindValue(14, reply.isOwn ? 1 : 0);

  if (!query.exec()) {
    qCritical() << "MessageStore insertTypedMessageValue failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
    return static_cast<quint64>(-1);
  }

  return query.lastInsertId().isValid() ? query.lastInsertId().toULongLong()
                                        : mid;
}

void DatabaseMessageStore::addMessage(quint64 mid, const QString& serverId,
                                      const QString& toPubKey,
                                      const QString& text, quint64 timestamp,
                                      bool isOwn, bool isCall, bool isSended,
                                      bool isReaded, bool isReceived,
                                      int timerValue,
                                      const ReplyInfo& reply) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  if (!query.prepare(
          "INSERT INTO messages (id, serverAddress, fromPubKey, type, text, t, "
          "isOwn, isSended, isReaded, isReceived, timerValue, replyTo, "
          "replyPreview, replyKind, replyIsOwn) VALUES (:id, "
          ":serverAddress, :fromPubKey, :type, :text, :t, :isOwn, :isSended, "
          ":isReaded, :isReceived, :timerValue, :replyTo, :replyPreview, "
          ":replyKind, :replyIsOwn)")) {
    qCritical() << "Prepare error in MessageStore addMessage:"
                << query.lastError().text();
    return;
  }

  query.bindValue(":id", mid);
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":fromPubKey", toPubKey);
  query.bindValue(
      ":type", static_cast<uint8_t>(isCall ? MessageModel::MESSAGE_TYPE::CALL
                                           : MessageModel::MESSAGE_TYPE::TEXT));
  query.bindValue(":text", text);
  query.bindValue(
      ":t", timestamp > 0
                ? timestamp
                : static_cast<quint64>(
                      QDateTime::currentDateTimeUtc().toSecsSinceEpoch()));
  query.bindValue(":isOwn", isOwn ? 1 : 0);
  query.bindValue(":isSended", isSended ? 1 : 0);
  query.bindValue(":isReaded", isReaded ? 1 : 0);
  query.bindValue(":isReceived", isReceived ? 1 : 0);
  query.bindValue(":timerValue", timerValue);
  // replyPreview / replyKind are TEXT NOT NULL. A default-constructed ReplyInfo
  // (any non-reply message) leaves these as *null* QStrings, which Qt binds as
  // SQL NULL and the NOT NULL constraint then rejects — silently dropping the
  // message. Coerce null to an empty (non-null) string so the insert always
  // satisfies the constraint.
  query.bindValue(":replyTo", static_cast<qulonglong>(reply.replyTo));
  query.bindValue(":replyPreview",
                  reply.preview.isNull() ? QString("") : reply.preview);
  query.bindValue(":replyKind", reply.kind.isNull() ? QString("") : reply.kind);
  query.bindValue(":replyIsOwn", reply.isOwn ? 1 : 0);

  if (!query.exec()) {
    qCritical() << "MessageStore addMessage failed:" << query.lastError().text()
                << "\nQuery:" << query.lastQuery();
  }
}

void DatabaseMessageStore::markMessagesAsReaded(
    const QString& serverId, const QString& contactPubKey) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "UPDATE messages SET isReaded = 1, readTime = :readTime WHERE "
      "serverAddress = :serverAddress AND fromPubKey = :fromPubKey AND "
      "isReaded = 0 AND isOwn = 0");
  query.bindValue(":readTime", QDateTime::currentDateTime().toSecsSinceEpoch());
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":fromPubKey", contactPubKey);
  if (!query.exec()) {
    qCritical() << "MessageStore markMessagesAsReaded failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}

void DatabaseMessageStore::markOwnMessagesAsReaded(
    const QString& serverId, const QString& contactPubKey) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "UPDATE messages SET isReaded = 1, readTime = :readTime WHERE "
      "serverAddress = :serverAddress AND fromPubKey = :fromPubKey AND "
      "isReaded = 0 AND isOwn = 1");
  query.bindValue(":readTime", QDateTime::currentDateTime().toSecsSinceEpoch());
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":fromPubKey", contactPubKey);
  if (!query.exec()) {
    qCritical() << "MessageStore markOwnMessagesAsReaded failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}

void DatabaseMessageStore::updateMessageSendedStatus(
    const QString& serverId, const QString& contactPubKey, quint64 id) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "UPDATE messages SET isSended = 1 WHERE serverAddress = :serverAddress "
      "AND fromPubKey = :fromPubKey AND id = :id");
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":fromPubKey", contactPubKey);
  query.bindValue(":id", id);
  if (!query.exec()) {
    qCritical() << "MessageStore updateMessageSendedStatus failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}

void DatabaseMessageStore::updateMessageDeliveredStatus(
    const QString& serverId, const QString& contactPubKey, quint64 id) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "UPDATE messages SET isReceived = 1 WHERE serverAddress = :serverAddress "
      "AND fromPubKey = :fromPubKey AND id = :id");
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":fromPubKey", contactPubKey);
  query.bindValue(":id", id);
  if (!query.exec()) {
    qCritical() << "MessageStore updateMessageDeliveredStatus failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}

void DatabaseMessageStore::updateCallDuration(const QString& serverId,
                                              const QString& contactPubKey,
                                              quint64 id,
                                              quint64 durationSec) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "UPDATE messages SET callDurationSec = :callDurationSec WHERE "
      "serverAddress = :serverAddress AND fromPubKey = :fromPubKey AND id = "
      ":id");
  query.bindValue(":callDurationSec", durationSec);
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":fromPubKey", contactPubKey);
  query.bindValue(":id", id);
  if (!query.exec()) {
    qCritical() << "MessageStore updateCallDuration failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}

void DatabaseMessageStore::deleteMessage(const QString& serverId,
                                         const QString& contactPubKey,
                                         quint64 id) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "DELETE FROM messages WHERE id = :id AND serverAddress = :serverAddress "
      "AND fromPubKey = :fromPubKey");
  query.bindValue(":id", id);
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":fromPubKey", contactPubKey);
  if (!query.exec()) {
    qCritical() << "MessageStore deleteMessage failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}

void DatabaseMessageStore::deleteAllMessagesFromContact(
    const QString& serverId, const QString& contactPubKey) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "DELETE FROM messages WHERE serverAddress = :serverAddress AND "
      "fromPubKey = :fromPubKey");
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":fromPubKey", contactPubKey);
  if (!query.exec()) {
    qCritical() << "MessageStore deleteAllMessagesFromContact failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}

QByteArray DatabaseMessageStore::loadMessageContentValue(
    const QString& serverId, const QString& contactPubKey,
    quint64 messageId) const {
  if (!ensureOpen()) {
    return QByteArray();
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "SELECT content FROM messages WHERE serverAddress = ? AND fromPubKey = ? "
      "AND id = ? LIMIT 1");
  query.bindValue(0, serverId);
  query.bindValue(1, contactPubKey);
  query.bindValue(2, static_cast<qulonglong>(messageId));
  if (!query.exec() || !query.next()) {
    return QByteArray();
  }

  return query.value(0).toByteArray();
}

bool DatabaseMessageStore::saveMessageContentValue(
    const QString& serverId, const QString& contactPubKey, quint64 id,
    const QByteArray& content) const {
  if (!ensureOpen()) {
    return false;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "UPDATE messages SET content = ? WHERE serverAddress = ? AND fromPubKey "
      "= ? AND id = ?");
  query.bindValue(0, content);
  query.bindValue(1, serverId);
  query.bindValue(2, contactPubKey);
  query.bindValue(3, static_cast<qulonglong>(id));
  return query.exec();
}

int DatabaseMessageStore::unreadCount(const QString& serverId,
                                      const QString& contactPubKey) const {
  if (!ensureOpen()) {
    return 0;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "SELECT COUNT(*) FROM messages WHERE serverAddress = ? AND fromPubKey = "
      "? AND isOwn = 0 AND isReaded = 0");
  query.bindValue(0, serverId);
  query.bindValue(1, contactPubKey);
  if (!query.exec()) {
    qCritical() << "MessageStore unreadCount failed:"
                << query.lastError().text();
    return 0;
  }

  return query.next() ? query.value(0).toInt() : 0;
}

Message DatabaseMessageStore::loadLastMessage(
    const QString& serverId, const QString& contactPubKey) const {
  Message message;
  if (!ensureOpen()) {
    return message;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "SELECT * FROM messages WHERE serverAddress = ? AND fromPubKey = ? ORDER "
      "BY t DESC LIMIT 1");
  query.bindValue(0, serverId);
  query.bindValue(1, contactPubKey);
  if (!query.exec()) {
    qCritical() << "MessageStore loadLastMessage failed:"
                << query.lastError().text();
    return message;
  }

  if (!query.next()) {
    return message;
  }

  return mapMessageRow(query);
}

QList<Message> DatabaseMessageStore::loadMessages(
    const QString& serverId, const QString& contactPubKey) const {
  QList<Message> messages;
  if (!ensureOpen()) {
    return messages;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "SELECT * FROM messages WHERE serverAddress = ? AND fromPubKey = ? ORDER "
      "BY t ASC");
  query.bindValue(0, serverId);
  query.bindValue(1, contactPubKey);
  if (!query.exec()) {
    qCritical() << "MessageStore loadMessages failed:"
                << query.lastError().text();
    return messages;
  }

  const QLocale locale = resolveMessageLocale();
  QString lastDate;
  while (query.next()) {
    Message message = mapMessageRow(query);
    message.isDate = false;

    const QDateTime dt = QDateTime::fromSecsSinceEpoch(message.t);
    const QString formatted = locale.toString(dt, "d MMMM");
    if (lastDate != formatted) {
      Message dateMessage;
      dateMessage.isDate = true;
      dateMessage.text = formatted;
      messages.append(dateMessage);
      lastDate = formatted;
    }

    messages.append(message);
  }

  return messages;
}