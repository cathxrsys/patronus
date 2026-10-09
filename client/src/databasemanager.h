#pragma once

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QQmlEngine>
#include <QRecursiveMutex>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QString>
#include <QThread>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>
#include <QtConcurrent>

#include "contactsmodel.h"
#include "databasecontactstore.h"
#include "databasemessagestore.h"
#include "databaseschemaservice.h"
#include "doubleratchet.h"
#include "filemessageutils.h"
#include "imageprovider.h"
#include "mainsignals.h"
#include "messagemodel.h"
#include "nlohmann/json.hpp"
#include "prekeys.h"
#include "qutils.h"

static constexpr int PBKDF2_ITERATIONS =
  4096000;  // High value for brute-force protection
static constexpr int KDF_ALGORITHM = 2;  // PBKDF2-HMAC-SHA512

class DatabaseWorker : public QObject {
  Q_OBJECT
 public:
  explicit DatabaseWorker(const QString& dbPath, QObject* parent = nullptr)
      : QObject(parent),
        m_contactStore(&m_database),
        m_messageStore(&m_database),
        m_dbPath(dbPath) {}

  bool executeQuery(const QString& query,
                    const QVariantList& params = QVariantList());
  // Erases every row from every account table (contacts, messages, prekeys,
  // identity keys, settings, ...) in a single transaction. Schema is preserved.
  bool wipeAllData();
  QVariantList selectRows(const QString& query,
                          const QVariantList& params = QVariantList());
  bool beginTransaction();
  bool commitTransaction();
  bool prekeyExistsRaw(const QByteArray& prekeyId);
  pqdh::KeyPairs loadPrekeyRaw(const QByteArray& prekeyId);
  dr::DoubleRatchet loadDoubleRatchetValue(const QString& serverId,
                                           const QString& pubKeyFingerprint);
  void saveDoubleRatchetValue(const QString& serverId,
                              const QString& pubKeyFingerprint,
                              const dr::DoubleRatchet& doubleRatchet);
  QByteArray loadMessageContentValue(const QString& serverId,
                                     const QString& contactPubKey,
                                     quint64 messageId);
  bool saveMessageContentValue(const QString& serverId,
                               const QString& contactPubKey, quint64 id,
                               const QByteArray& content);
  quint64 insertTypedMessageValue(quint64 mid, const QString& serverId,
                                  const QString& toPubKey, const QString& text,
                                  MessageModel::MESSAGE_TYPE type,
                                  quint64 timestamp, bool isOwn, bool isSended,
                                  bool isReaded, bool isReceived,
                                  const QByteArray& content = QByteArray(),
                                  const ReplyInfo& reply = {});
  QStringList loadKnownServerAddressesValue();
  bool storeAnonymousContactSessionValue(const QString& serverId,
                                         const QString& contactPubKey,
                                         const QString& doubleRatchetJson);
  bool contactExistsValue(const QString& serverId,
                          const QString& pubKeyFingerprint);

 public slots:
  void setDatabasePath(const QString& dbPath) {
    m_dbPath = dbPath;
    m_databasePath = dbPath;
  }

  // This slot is called on a separate thread
  bool initDatabase(const QString& password) {
    if (m_database.isOpen()) {
      closeDatabase();
    }

    m_databasePath = m_dbPath;

    QFileInfo fileInfo(m_dbPath);
    QDir dir = fileInfo.dir();
    if (!dir.exists()) {
      dir.mkpath(".");
    }

    m_database = QSqlDatabase::addDatabase(
        "QSQLITE",
        "WorkerConnection" + QUuid::createUuid().toString());  // QSQLCIPHER
    m_database.setDatabaseName(m_dbPath);

    if (!m_database.open()) {
      return false;
    }

    if (!password.isEmpty()) {
      QSqlQuery query(m_database);

      // Set encryption parameters for brute-force protection
      query.exec(QStringLiteral("PRAGMA cipher_compatibility = 4"));
      query.exec(QStringLiteral("PRAGMA kdf_iter = %1").arg(PBKDF2_ITERATIONS));

      // Set the encryption key
      query.exec(QStringLiteral("PRAGMA key = '%1'").arg(password));

      // Verify that the key is correct
      if (!query.exec("SELECT count(*) FROM sqlite_master")) {
        m_database.close();
        QSqlDatabase::removeDatabase("WorkerConnection");
        return false;
      }

      m_isEncrypted = true;
    } else {
      m_isEncrypted = false;
    }

    // Ensure the schema (incl. migrations like the reply columns) on THIS
    // worker connection — every message read/write goes through m_messageStore
    // on m_database, so if this connection's file view is missing a migrated
    // column the INSERT silently fails and the message never persists.
    DatabaseSchemaService schemaService;
    QString schemaError;
    if (!schemaService.ensureSchema(&m_database, &schemaError) &&
        !schemaError.isEmpty()) {
      qCritical() << "[DatabaseWorker] ensureSchema failed:" << schemaError;
    }

    return true;
  }

  void reInitDatabase(const QString& newPassword) { initDatabase(newPassword); }

  bool closeDatabase() {
    if (m_database.isOpen()) {
      m_database.close();
    }

    QSqlDatabase::removeDatabase("WorkerConnection");

    return true;
  }

  int getContactTimerValue(const QString& serverId,
                           const QString& contactPubKey) {
    return m_contactStore.contactTimerValue(serverId, contactPubKey);
  }

  // Connected to DatabaseManager::sigAddMessage (call messages, async), so its
  // arity must match that signal — do NOT add parameters here. Reply-carrying
  // inserts go through addMessageWithReply() below instead.
  void addMessage(const quint64 mid, const QString& serverId,
                  const QString& toPubKey, const QString& text,
                  quint64 timestamp, bool isOwn, bool isMsg, bool isSended,
                  bool isReaded, bool isReceived, bool isCall) {
    Q_UNUSED(isMsg);
    m_messageStore.addMessage(mid, serverId, toPubKey, text, timestamp, isOwn,
                              isCall, isSended, isReaded, isReceived,
                              getContactTimerValue(serverId, toPubKey));
  }

  void addMessageWithReply(const quint64 mid, const QString& serverId,
                           const QString& toPubKey, const QString& text,
                           quint64 timestamp, bool isOwn, bool isCall,
                           bool isSended, bool isReaded, bool isReceived,
                           const ReplyInfo& reply, int timerOverride = -1) {
    // The disappearing-message TTL is a property of the message, not of the
    // local contact: a received message carries the sender's timer in its
    // payload (timerOverride >= 0) so both peers persist an identical
    // timerValue and the per-second sweep expires the same messages on both
    // sides. Locally-sent messages and legacy peers that omit the field fall
    // back to this device's current contact timer.
    const int timerValue = timerOverride >= 0
                               ? timerOverride
                               : getContactTimerValue(serverId, toPubKey);
    m_messageStore.addMessage(mid, serverId, toPubKey, text, timestamp, isOwn,
                              isCall, isSended, isReaded, isReceived, timerValue,
                              reply);
  }

  void markMessagesAsReaded(const QString& serverId,
                            const QString& contactPubKey) {
    m_messageStore.markMessagesAsReaded(serverId, contactPubKey);
  }

  void markOwnMessagesAsReaded(const QString& serverId,
                               const QString& contactPubKey) {
    m_messageStore.markOwnMessagesAsReaded(serverId, contactPubKey);
  }

  void updateMessageSendedStatus(const QString& serverId,
                                 const QString& contactPubKey,
                                 const quint64 id) {
    m_messageStore.updateMessageSendedStatus(serverId, contactPubKey, id);
  }

  void updateMessageDeliveredStatus(const QString& serverId,
                                    const QString& contactPubKey,
                                    const quint64 id) {
    m_messageStore.updateMessageDeliveredStatus(serverId, contactPubKey, id);
  }

  void updateCallDuration(const QString& serverId,
                          const QString& contactPubKey, const quint64 id,
                          quint64 durationSec) {
    m_messageStore.updateCallDuration(serverId, contactPubKey, id,
                                      durationSec);
  }

  void deleteMessage(const QString& serverId, const QString& contactPubKey,
                     const quint64 id) {
    m_messageStore.deleteMessage(serverId, contactPubKey, id);
  }

  void deleteAllMessagesFromContact(const QString& serverId,
                                    const QString& contactPubKey) {
    m_messageStore.deleteAllMessagesFromContact(serverId, contactPubKey);
  }

  void settingSetBool(const QString& key, bool value) {
    if (!m_database.isOpen()) {
      if (!m_database.open()) {
        qCritical() << "Worker: DB not open and cannot reopen!";
        return;
      }
    }

    QSqlQuery q(m_database);
    if (!q.prepare("INSERT OR REPLACE INTO boolsettings (name, value) VALUES "
                   "(:name, :value)")) {
      qCritical() << "Prepare error in settingSetBool:" << q.lastError().text();
      return;
    }
    q.bindValue(":name", key);
    q.bindValue(":value", value ? "1" : "0");

    if (!q.exec()) {
      qCritical() << "Worker settingSetBool failed:" << q.lastError().text()
                  << "\nQuery:" << q.lastQuery();
    }
  }

  void settingSetText(const QString& key, const QString& value) {
    if (!m_database.isOpen()) {
      if (!m_database.open()) {
        qCritical() << "Worker: DB not open and cannot reopen!";
        return;
      }
    }

    QSqlQuery q(m_database);
    if (!q.prepare("INSERT OR REPLACE INTO textsettings (name, value) VALUES "
                   "(:name, :value)")) {
      qCritical() << "Prepare error in settingSetText:" << q.lastError().text();
      return;
    }
    q.bindValue(":name", key);
    q.bindValue(":value", value);

    if (!q.exec()) {
      qCritical() << "Worker settingSetText failed:" << q.lastError().text()
                  << "\nQuery:" << q.lastQuery();
    }
  }

  void settingSetBlob(const QString& key, const QByteArray& value) {
    if (!m_database.isOpen()) {
      if (!m_database.open()) {
        qCritical() << "Worker: DB not open and cannot reopen!";
        return;
      }
    }

    QSqlQuery q(m_database);
    if (!q.prepare("INSERT OR REPLACE INTO blobsettings (name, value) VALUES "
                   "(:name, :value)")) {
      qCritical() << "Prepare error in settingSetBlob:" << q.lastError().text();
      return;
    }
    q.bindValue(":name", key);
    q.bindValue(":value", value);

    if (!q.exec()) {
      qCritical() << "Worker settingSetBlob failed:" << q.lastError().text()
                  << "\nQuery:" << q.lastQuery();
    }
  }

  void onMessageSended(const QString& mid, const QString& serverId,
                       const QString& contactPubKey, const QString& messageText,
                       quint64 timestamp, quint64 replyTo,
                       const QString& replyPreview, const QString& replyKind,
                       bool replyIsOwn) {
    qDebug() << "[DatabaseWorker] onMessageSended called with mid:" << mid
             << "serverId:" << serverId << "contactPubKey:" << contactPubKey
             << "messageText:" << messageText << "timestamp:" << timestamp;
    addMessageWithReply(mid.toULongLong(), serverId, contactPubKey, messageText,
                        timestamp, true, false, false, false, false,
                        ReplyInfo{replyTo, replyPreview, replyKind, replyIsOwn});
  }

  void onMessageStatusDelivered(const QString& mid, const QString& serverId,
                                const QString& contactPubKey) {
    qDebug() << "[DatabaseWorker] onMessageStatusDelivered called with mid:"
             << mid << "serverId:" << serverId
             << "contactPubKey:" << contactPubKey;
    updateMessageSendedStatus(serverId, contactPubKey, mid.toULongLong());
    updateMessageDeliveredStatus(serverId, contactPubKey, mid.toULongLong());
  }

  void onMessageStatusSended(const QString& mid, const QString& serverId,
                             const QString& contactPubKey) {
    qDebug() << "[DatabaseWorker] onMessageStatusSended called with mid:" << mid
             << "serverId:" << serverId << "contactPubKey:" << contactPubKey;
    updateMessageSendedStatus(serverId, contactPubKey, mid.toULongLong());
  }

  void onLastOnlineChanged(const QString& serverId,
                           const QString& contactPubKey,
                           const QString& lastOnline) {
    m_contactStore.updateLastOnline(serverId, contactPubKey, lastOnline);
  }

  void onE2eTextMessageReceived(const QString& serverId,
                                const QString& fromPubKey,
                                const QString& messageText, quint64 messageId,
                                quint64 timestamp, quint64 replyTo,
                                const QString& replyPreview,
                                const QString& replyKind, bool replyIsOwn,
                                int timerSeconds = -1) {
    qDebug()
        << "[DatabaseWorker] onE2eTextMessageReceived called with serverId:"
        << serverId << "fromPubKey:" << fromPubKey
        << "messageText:" << messageText << "messageId:" << messageId
        << "timestamp:" << timestamp << "timerSeconds:" << timerSeconds;
    addMessageWithReply(messageId, serverId, fromPubKey, messageText, timestamp,
                        false, false, false, false, false,
                        ReplyInfo{replyTo, replyPreview, replyKind, replyIsOwn},
                        timerSeconds);
  }

  void sendOnlineRequest() {
    // qDebug() << "[DatabaseWorker] sendOnlineRequest called.";

    if (!m_database.isOpen()) {
      if (!m_database.open()) {
        qCritical() << "Worker: DB not open and cannot reopen!";
        return;
      }
    }

    QSqlQuery query(m_database);

    if (query.exec("SELECT serverAddress, pubKey FROM contacts")) {
      while (query.next()) {
        QString server = query.value(0).toString();
        QString contactPubKey = query.value(1).toString();
        MainSignals::instance().emitOnlineRequest(server, contactPubKey);
      }
    } else {
      qCritical() << "sendOnlineRequest: SELECT error:"
                  << query.lastError().text();
    }
  }

  void deleteContactByPubKey(const QString& serverId,
                             const QString& contactPubKey) {
    m_contactStore.deleteContactByPubKey(serverId, contactPubKey);
  }

  void onTimerTicked() {
    // qDebug() << "[DatabaseWorker] onTimerTicked called.";

    sendOnlineRequest();
  }

  void onAllMessagesReaded(const QString& serverId,
                           const QString& contactPubKey) {
    qDebug() << "[DatabaseWorker] onAllMessagesReaded called with serverId:"
             << serverId << "contactPubKey:" << contactPubKey;
    markMessagesAsReaded(serverId, contactPubKey);
  }

  void onAllOwnMessagesReaded(const QString& serverId,
                              const QString& contactPubKey) {
    qDebug() << "[DatabaseWorker] onAllOwnMessagesReaded called with serverId:"
             << serverId << "contactPubKey:" << contactPubKey;
    markOwnMessagesAsReaded(serverId, contactPubKey);
  }

  void onSettingSetBool(const QString& key, bool value) {
    qDebug() << "[DatabaseWorker] onSettingSetBool called with key:" << key
             << "value:" << value;
    settingSetBool(key, value);
  }

  void onSettingSetText(const QString& key, const QString& value) {
    qDebug() << "[DatabaseWorker] onSettingSetText called with key:" << key
             << "value:" << value;
    settingSetText(key, value);
  }

  void onSettingSetBlob(const QString& key, const QByteArray& value) {
    qDebug() << "[DatabaseWorker] onSettingSetBlob called with key:" << key
             << "value size:" << value.size();
    settingSetBlob(key, value);
  }

  void onAvatarDeleted() { onSettingSetBlob("avatar", QByteArray()); }

  void onMessageDelete(const quint64 message_id, const QString& serverId,
                       const QString& contactPubKey) {
    qDebug() << "[DatabaseWorker] onMessageDelete called with message_id:"
             << message_id << "serverId:" << serverId
             << "contactPubKey:" << contactPubKey;
    deleteMessage(serverId, contactPubKey, message_id);
  }

  void onContactRemove(const QString& serverId, const QString& contactPubKey) {
    qDebug() << "[DatabaseWorker] onContactRemove called with contactPubKey:"
             << contactPubKey << "on server:" << serverId;
    deleteAllMessagesFromContact(serverId, contactPubKey);
    deleteContactByPubKey(serverId, contactPubKey);
  }

  void onIdentityRevoked(const QString& serverId,
                         const QString& contactPubKey) {
    qDebug() << "[DatabaseWorker] onIdentityRevoked called with contactPubKey:"
             << contactPubKey << "on server:" << serverId;
    m_contactStore.markContactRevoked(serverId, contactPubKey);
  }

  // qDebug() << "[DatabaseManager] [setDoubleRatchet] Saving DoubleRatchet for
  // contact" << pubKeyFingerprint;

  // if (!m_database.isOpen()) {
  //     setLastError(tr("Database is not open"));
  //     return;
  // }

  // nlohmann::json doubleRatchetJson = doubleRatchet.to_json();
  // QString doubleRatchetJsonString =
  // QString::fromStdString(doubleRatchetJson.dump());

  // QSqlQuery q(m_database);
  // q.prepare("UPDATE contacts SET doubleratchet = ? WHERE pubKey = ?");
  // q.bindValue(0, doubleRatchetJsonString);
  // q.bindValue(1, pubKeyFingerprint);

  // if (!q.exec()) {
  //     setLastError(q.lastError().text());
  //     return;
  // }

  // this->commitTransaction();

  void onDoubleRatchetUpdate(const QString& serverId,
                             const QString& pubKeyFingerprint,
                             const nlohmann::json& doubleRatchetJson) {
    qDebug() << "[DatabaseWorker] onDoubleRatchetUpdate called with serverId:"
             << serverId << "pubKeyFingerprint:" << pubKeyFingerprint;

    if (!m_database.isOpen()) {
      if (!m_database.open()) {
        qCritical() << "Worker: DB not open and cannot reopen!";
        return;
      }
    }

    QSqlQuery q(m_database);
    q.prepare(
        "UPDATE contacts SET doubleratchet = ? WHERE serverAddress = ? AND "
        "pubKey = ?");
    q.bindValue(0, QString::fromStdString(doubleRatchetJson.dump()));
    q.bindValue(1, serverId);
    q.bindValue(2, pubKeyFingerprint);

    if (!q.exec()) {
      qCritical() << "Worker onDoubleRatchetUpdate failed:"
                  << q.lastError().text() << "\nQuery:" << q.lastQuery();
      return;
    }
    qDebug() << "[DatabaseWorker] onDoubleRatchetUpdate succeeded for "
                "pubKeyFingerprint:"
             << pubKeyFingerprint;
  }

  void onUpdateMessageDeliveredStatus(const QString& serverId,
                                      const QString& contactPubKey,
                                      const quint64 messageId) {
    qDebug() << "[DatabaseManager] [updateMessageDeliveredStatus] Updating "
                "message delivered status for message ID"
             << messageId << "contact:" << serverId << contactPubKey;

    if (!m_database.isOpen()) {
      qCritical() << "Database is not open";
      return;
    }

    QSqlQuery q(m_database);
    q.prepare(
        "UPDATE messages SET isReceived = 1 WHERE id = ? AND serverAddress = ? "
        "AND fromPubKey = ?");
    q.bindValue(0, messageId);
    q.bindValue(1, serverId);
    q.bindValue(2, contactPubKey);

    if (!q.exec()) {
      qCritical() << "Failed to update message delivered status:"
                  << q.lastError().text() << "\nQuery:" << q.lastQuery();
      return;
    }
  }

  void onMessageReaded(const QString& serverId, const QString& fromPubKey,
                       const quint64 messageId) {
    qDebug() << "[DatabaseManager] [onMessageReaded] Marking message as readed "
                "for message ID"
             << messageId << "from contact:" << fromPubKey
             << "on server:" << serverId;

    if (!m_database.isOpen()) {
      qCritical() << "Database is not open";
      return;
    }

    QSqlQuery q(m_database);
    q.prepare(
        "UPDATE messages SET isReaded = 1, readTime = ? WHERE id = ? AND "
        "serverAddress = ? AND fromPubKey = ?");
    q.bindValue(0, QDateTime::currentDateTime().toSecsSinceEpoch());
    q.bindValue(1, messageId);
    q.bindValue(2, serverId);
    q.bindValue(3, fromPubKey);

    if (!q.exec()) {
      qCritical() << "Failed to mark message as readed:" << q.lastError().text()
                  << "\nQuery:" << q.lastQuery();
      return;
    }
  }

  void onClearHistory(const QString& serverId, const QString& contactPubKey) {
    qDebug()
        << "[DatabaseManager] [onClearHistory] Clearing history for contact:"
        << contactPubKey << "on server:" << serverId;
    deleteAllMessagesFromContact(serverId, contactPubKey);
  }

  void updateContactTimerValue(const QString& serverId,
                               const QString& contactPubKey, int timerValue) {
    qDebug() << "[DatabaseManager] [updateContactTimerValue] Updating timer "
                "value for contact:"
             << contactPubKey << "to" << timerValue << "seconds";
    m_contactStore.updateContactTimerValue(serverId, contactPubKey, timerValue);
  }

  Message getLastMessage(const QString& serverId,
                         const QString& contactPubKey) {
    return m_messageStore.loadLastMessage(serverId, contactPubKey);
  }

  QString getTimerValueS(int timerValue) {
    QString minutes = tr("minutes");
    QString minute = tr("minute");
    QString seconds = tr("seconds");
    QString second = tr("second");
    QString hours = tr("hours");
    QString hour = tr("hour");
    QString days = tr("days");
    QString day = tr("day");

    if (timerValue < 60) {
      return QString::number(timerValue) + " " +
             (timerValue == 1 ? second : seconds);
    } else if (timerValue < 3600) {
      int minutesValue = timerValue / 60;
      return QString::number(minutesValue) + " " +
             (minutesValue == 1 ? minute : minutes);
    } else if (timerValue < 86400) {
      int hoursValue = timerValue / 3600;
      return QString::number(hoursValue) + " " +
             (hoursValue == 1 ? hour : hours);
    } else {
      int daysValue = timerValue / 86400;
      return QString::number(daysValue) + " " + (daysValue == 1 ? day : days);
    }
    return "";
  }

  void onSetTimer(const QString& serverId, const QString& contactPubKey,
                  int seconds) {
    updateContactTimerValue(serverId, contactPubKey, seconds);
    Message lastMessage = this->getLastMessage(serverId, contactPubKey);
    QString text;
    if (seconds == 0) {
      text = tr("Timer is disabled");
    } else {
      text = tr("Timer is set for %1").arg(this->getTimerValueS(seconds));
    }

    this->onAddInfoMessage(serverId, contactPubKey, text, lastMessage.id + 1);
  }

  void onAddInfoMessage(const QString& serverId, const QString& toPubKey,
                        const QString& text, quint64 messageId) {
    qDebug() << "[DatabaseWorker] onAddInfoMessage called with messageId:"
             << messageId << "and text:" << text;

    if (!m_database.isOpen()) {
      qCritical() << "Worker: DB not open and cannot reopen!";
      return;
    }

    QSqlQuery q(m_database);

    QDateTime currentDateTime = QDateTime::currentDateTime();
    quint64 timestamp =
        static_cast<quint64>(currentDateTime.toSecsSinceEpoch());

    q.prepare(
        "INSERT INTO messages (id, serverAddress, fromPubKey, type, text, t, "
        "isOwn, isSended, isReaded, isReceived) VALUES (?, ?, ?, ?, ?, ?, ?, "
        "?, ?, ?)");
    q.bindValue(0, messageId);
    q.bindValue(1, serverId);
    q.bindValue(2, toPubKey);
    q.bindValue(3, static_cast<uint8_t>(MessageModel::MESSAGE_TYPE::INFO));
    q.bindValue(4, text);
    q.bindValue(5, timestamp);
    q.bindValue(6, 1);
    q.bindValue(7, 1);
    q.bindValue(8, 1);
    q.bindValue(9, 1);

    if (!q.exec()) {
      qCritical() << "Worker Insert info message failed:"
                  << q.lastError().text() << "\nQuery:" << q.lastQuery();
      return;
    }
  }

  void onSecondTimerTicked() {
    // qDebug() << "[DatabaseWorker] onSecondTimerTicked called.";

    if (!m_database.isOpen()) {
      if (!m_database.open()) {
        qCritical() << "Worker: DB not open and cannot reopen!";
        return;
      }
    }

    QSqlQuery query(m_database);
    if (query.exec("SELECT id, serverAddress, fromPubKey, timerValue, readTime "
                   "FROM messages WHERE timerValue > 0 AND readTime > 0 AND "
                   "readTime <= strftime('%s', 'now') - timerValue + 1")) {
      while (query.next()) {
        quint64 messageId = query.value(0).toULongLong();
        QString serverId = query.value(1).toString();
        QString contactPubKey = query.value(2).toString();
        int timerValue = query.value(3).toInt();
        int readTime = query.value(4).toInt();

        Q_UNUSED(timerValue);
        Q_UNUSED(readTime);

        MainSignals::instance().emitLocalMessageDelete(messageId, serverId,
                                                       contactPubKey);
      }

    } else {
      qCritical() << "Failed to select messages for timer check:"
                  << query.lastError().text();
    }
  }

  void loadContacts();
  void loadMessages(const QString& serverId, const QString& contactPubKey);
  void updateContactRecord(const QString& serverId,
                           const QString& contactPubKey,
                           const QString& firstName, const QString& lastName,
                           const QString& aboutMe, const QString& nameStyle,
                           const QByteArray& avatarBlob);

 signals:
  void contactsLoaded(const QList<Contact>& contacts);
  void messagesLoaded(const QString& serverId, const QString& contactPubKey,
                      const QList<Message>& messages);

 private:
  DatabaseContactStore m_contactStore;
  DatabaseMessageStore m_messageStore;
  QString m_dbPath;
  QSqlDatabase m_database;
  bool m_isEncrypted;
  QString m_databasePath;
};

class DatabaseManager : public QObject {
  Q_OBJECT

  Q_PROPERTY(bool isOpen READ isOpen NOTIFY isOpenChanged)
  Q_PROPERTY(bool isEncrypted READ isEncrypted NOTIFY isEncryptedChanged)
  Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
  Q_PROPERTY(QString databasePath READ databasePath NOTIFY databasePathChanged)

 public:
  explicit DatabaseManager(QObject* parent = nullptr,
                           AvatarProvider* avatarProvider = nullptr);
  ~DatabaseManager();

  Q_INVOKABLE bool openDatabase(const QString& path,
                                const QString& password = QString());
  Q_INVOKABLE bool closeDatabase();
  Q_INVOKABLE bool changePassword(const QString& newPassword);

  Q_INVOKABLE bool execute(const QString& query,
                           const QVariantList& params = QVariantList());
  // Factory-reset: erase all account data (contacts, messages, keys, settings).
  Q_INVOKABLE bool wipeAllData();
  Q_INVOKABLE QVariantList select(const QString& query,
                                  const QVariantList& params = QVariantList());
  Q_INVOKABLE QVariant selectOne(const QString& query,
                                 const QVariantList& params = QVariantList());

  Q_INVOKABLE bool beginTransaction();
  Q_INVOKABLE bool commitTransaction();

  Q_INVOKABLE bool contactExists(const QString& serverId,
                                 const QString& pubKeyFingerprint);

  Q_INVOKABLE void requestContactsLoad();
  Q_INVOKABLE void requestMessagesLoad(const QString& serverId,
                                       const QString& contactPubKey);
  Q_INVOKABLE void requestContactRemoval(const QString& serverId,
                                         const QString& contactPubKey);
  Q_INVOKABLE void requestContactUpdate(const QString& serverId,
                                        const QString& contactPubKey,
                                        const QString& firstName,
                                        const QString& lastName,
                                        const QString& aboutMe,
                                        const QString& nameStyle,
                                        const QByteArray& avatarBlob);

  Q_INVOKABLE quint64 addTypedMessage(
      const quint64 mid, const QString& serverId, const QString& toPubKey,
      const QString& text, MessageModel::MESSAGE_TYPE type, quint64 timestamp,
      bool isOwn, bool isSended, bool isReaded, bool isReceived,
      const QByteArray& content = QByteArray(), const ReplyInfo& reply = {});
  Q_INVOKABLE quint64 addMessage(const quint64 mid, const QString& serverId,
                                 const QString& toPubKey, const QString& text,
                                 quint64 timestamp, bool isOwn, bool isMsg,
                                 bool isSended, bool isReaded, bool isReceived,
                                 bool isCall);
  Q_INVOKABLE quint64 addInfoMessage(const QString& serverId,
                                     const QString& toPubKey,
                                     const QString& text);
  Q_INVOKABLE void NEW_THREAD_addMessage(
      const quint64 mid, const QString& serverId, const QString& toPubKey,
      const QString& text, quint64 timestamp, bool isOwn, bool isMsg,
      bool isSended, bool isReaded, bool isReceived, bool isCall);

  dr::DoubleRatchet getDoubleRatchet(const QString& serverId,
                                     const QString& pubKeyFingerprint);
  void setDoubleRatchet(const QString& serverId,
                        const QString& pubKeyFingerprint,
                        const dr::DoubleRatchet& doubleRatchet);
  bool prekeyExists(const QByteArray& prekeyId);
  bool prekeyExists(const QString& prekeyId);
  bool prekeyExists(const std::vector<uint8_t>& prekeyId);
  pqdh::KeyPairs getPrekey(const QByteArray& prekeyId);
  pqdh::KeyPairs getPrekey(const QString& prekeyId);
  pqdh::KeyPairs getPrekey(const std::vector<uint8_t>& prekeyId);

  Message getLastMessage(const QString& serverId, const QString& contactPubKey);
  Q_INVOKABLE QVariantMap getLastMessageMap(const QString& serverId,
                                            const QString& contactPubKey);
  Q_INVOKABLE QByteArray getMessageContent(const QString& serverId,
                                           const QString& contactPubKey,
                                           const quint64 messageId);

  Q_INVOKABLE int getUnreadedCount(const QString& serverId,
                                   const QString& contactPubKey);
  Q_INVOKABLE int getUnreadCount(const QString& serverId,
                                 const QString& contactPubKey);

  Q_INVOKABLE void updateMessageSendedStatus(const QString& serverId,
                                             const QString& contactPubKey,
                                             const quint64 id);
  Q_INVOKABLE void updateMessageSentStatus(const QString& serverId,
                                           const QString& contactPubKey,
                                           const quint64 id);
  Q_INVOKABLE void updateMessageDeliveredStatus(const QString& serverId,
                                                const QString& contactPubKey,
                                                const quint64 id);
  Q_INVOKABLE void updateCallDuration(const QString& serverId,
                                      const QString& contactPubKey,
                                      const quint64 id, quint64 durationSec);
  Q_INVOKABLE bool updateMessageContent(const QString& serverId,
                                        const QString& contactPubKey,
                                        const quint64 id,
                                        const QByteArray& content);

  void storePendingFileUpload(const QString& serverId,
                              const QString& contactPubKey, quint64 messageId,
                              int messageType, const QString& encryptedPath,
                              const QByteArray& fileData, quint64 timestamp,
                              const QString& previewText);
  QVariantList loadPendingFileUploads(const QString& serverId);
  QVariantList loadPendingFileUploadsForContact(const QString& serverId,
                                                const QString& contactPubKey);
  QString pendingFileUploadEncryptedPath(quint64 messageId,
                                         const QString& serverId);
  void deletePendingFileUpload(quint64 messageId, const QString& serverId);

  // Own album messages not yet confirmed delivered (isSended = 0), so a
  // failed/interrupted send can be resent from the locally stored `content`
  // on the next reconnect instead of being lost. See MESSAGE_TYPE::ALBUM.
  QVariantList loadPendingAlbumMessages(const QString& serverId);

  // Albums composed while offline: the whole album (encrypted blobs still to
  // upload + assembly metadata) is stored as one row and finished on the next
  // reconnect. See FileTransferManager::retryPendingAlbumUploads.
  void storePendingAlbumUpload(quint64 albumMessageId, const QString& serverId,
                               const QString& contactPubKey,
                               const QString& caption, quint64 timestamp,
                               const ReplyInfo& reply, int totalItems,
                               const QByteArray& itemsJson);
  QVariantList loadPendingAlbumUploads(const QString& serverId);
  void deletePendingAlbumUpload(quint64 albumMessageId,
                                const QString& serverId);

  Q_INVOKABLE QString getTimerValueS(int timerValue);

  // Properties
  bool isOpen() const;
  bool isEncrypted() const;
  QString lastError() const;
  QString databasePath() const;

 signals:
  void isOpenChanged();
  void isEncryptedChanged();
  void lastErrorChanged();
  void databasePathChanged();
  void databasePathResolved(const QString& path);
  void databaseOpened(const QString& password);
  void databaseClosed();
  // Emitted after wipeAllData() reseeds default settings rows, so
  // SettingsManager can refresh its cache without needing an app restart
  // (deliberately separate from databaseOpened, which also re-triggers the
  // worker's initDatabase()).
  void defaultsReseeded();
  void passwordChanged();
  void passwordChangedWithNewPassword(const QString& newPassword);
  void errorOccurred(const QString& error);
  void contactsLoaded(const QList<Contact>& contacts);
  void messagesLoaded(const QString& serverId, const QString& contactPubKey,
                      const QList<Message>& messages);

  void sigAddMessage(quint64 mid, QString serverId, QString toPubKey,
                     QString text, quint64 timestamp, bool isOwn, bool isMsg,
                     bool isSended, bool isReaded, bool isReceived,
                     bool isCall);
  void sigMarkMessagesAsReaded(const QString& serverId,
                               const QString& contactPubKey);
  void sigUpdateMessageSendedStatus(const QString& serverId,
                                    const QString& contactPubKey,
                                    const quint64 id);
  void sigUpdateMessageDeliveredStatus(const QString& serverId,
                                       const QString& contactPubKey,
                                       const quint64 id);
  void sigUpdateCallDuration(const QString& serverId,
                             const QString& contactPubKey, const quint64 id,
                             quint64 durationSec);
  void sigInitWorker();  // Initialization signal
  void sigLoadContacts();
  void sigLoadMessages(const QString& serverId, const QString& contactPubKey);
  void sigDeleteContact(const QString& serverId, const QString& contactPubKey);
  void sigUpdateContact(const QString& serverId, const QString& contactPubKey,
                        const QString& firstName, const QString& lastName,
                        const QString& aboutMe, const QString& nameStyle,
                        const QByteArray& avatarBlob);

 private:
  QHash<QString, dr::DoubleRatchet> m_doubleRatchetCache;

  void setLastError(const QString& error);
  QString generateConnectionName() const;
  bool configureCipher();
  void enrichContactsWithAvatars(QList<Contact>* contacts);

  QSqlDatabase m_database;
  QString m_connectionName;
  QString m_databasePath;
  QString m_lastError;
  bool m_isEncrypted;

  QThread* m_workerThread;
  DatabaseWorker* m_worker;

  QString m_password;

  mutable QRecursiveMutex mutex_;

  void ensureTablesExist();

 public:
  AvatarProvider* m_avatarProvider;
};