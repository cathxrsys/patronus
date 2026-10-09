#include "databasemanager.h"

#include <algorithm>

#include "accountmanager.h"
#include "databaseschemaservice.h"
#include "qutils.h"
#include "voicecliputils.h"

namespace {

QString identityKey(const QString& serverId, const QString& pubKey) {
  return serverId + QLatin1Char('\x1f') + pubKey;
}

void restoreAudioWaveformIfNeeded(FileMessageData* fileData) {
  if (fileData == nullptr || !filemessage::isAudioMessage(*fileData) ||
      !fileData->waveform.isEmpty() || fileData->localPath.isEmpty()) {
    return;
  }

  fileData->waveform = voiceclip::buildWaveformFromFile(fileData->localPath);
}

template <typename Func>
auto runWorkerBlocking(DatabaseWorker* worker,
                       Func&& func) -> decltype(func()) {
  using ReturnType = decltype(func());
  ReturnType result{};

  if (worker == nullptr) {
    return result;
  }

  if (QThread::currentThread() == worker->thread()) {
    return func();
  }

  QMetaObject::invokeMethod(
      worker, [&result, &func]() { result = func(); },
      Qt::BlockingQueuedConnection);
  return result;
}

template <typename Func>
void runWorkerBlockingVoid(DatabaseWorker* worker, Func&& func) {
  if (worker == nullptr) {
    return;
  }

  if (QThread::currentThread() == worker->thread()) {
    func();
    return;
  }

  QMetaObject::invokeMethod(worker, std::forward<Func>(func),
                            Qt::BlockingQueuedConnection);
}

}  // namespace

bool DatabaseWorker::executeQuery(const QString& query,
                                  const QVariantList& params) {
  if (!m_database.isOpen() && !m_database.open()) {
    qCritical() << "Worker: DB not open and cannot reopen!";
    return false;
  }

  QSqlQuery q(m_database);
  q.prepare(query);
  for (int index = 0; index < params.size(); ++index) {
    q.bindValue(index, params.at(index));
  }

  if (!q.exec()) {
    qCritical() << "Worker executeQuery failed:" << q.lastError().text()
                << "\nQuery:" << q.lastQuery();
    return false;
  }

  return true;
}

bool DatabaseWorker::wipeAllData() {
  if (!m_database.isOpen() && !m_database.open()) {
    qCritical() << "Worker: DB not open, cannot wipe!";
    return false;
  }

  static const char* const tables[] = {
      "messages",     "contacts",     "prekeys",
      "blobvalues",   "textsettings", "boolsettings",
      "intsettings",  "blobsettings", "pending_file_uploads",
      "pending_album_uploads"};

  m_database.transaction();
  QSqlQuery q(m_database);
  for (const char* const table : tables) {
    if (!q.exec(QStringLiteral("DELETE FROM ") + QLatin1String(table))) {
      qCritical() << "Worker wipeAllData failed on" << table
                  << q.lastError().text();
      m_database.rollback();
      return false;
    }
  }
  m_database.commit();
  qDebug() << "[DatabaseWorker] wipeAllData: all account data erased";
  return true;
}

QVariantList DatabaseWorker::selectRows(const QString& query,
                                        const QVariantList& params) {
  QVariantList results;

  if (!m_database.isOpen() && !m_database.open()) {
    qCritical() << "Worker: DB not open and cannot reopen!";
    return results;
  }

  QSqlQuery q(m_database);
  q.prepare(query);
  for (int index = 0; index < params.size(); ++index) {
    q.bindValue(index, params.at(index));
  }

  if (!q.exec()) {
    qCritical() << "Worker selectRows failed:" << q.lastError().text()
                << "\nQuery:" << q.lastQuery();
    return results;
  }

  while (q.next()) {
    QVariantMap row;
    for (int column = 0; column < q.record().count(); ++column) {
      row[q.record().fieldName(column)] = q.value(column);
    }
    results.append(row);
  }

  return results;
}

bool DatabaseWorker::beginTransaction() {
  if (!m_database.isOpen() && !m_database.open()) {
    qCritical() << "Worker: DB not open and cannot reopen!";
    return false;
  }

  return m_database.transaction();
}

bool DatabaseWorker::commitTransaction() {
  if (!m_database.isOpen() && !m_database.open()) {
    qCritical() << "Worker: DB not open and cannot reopen!";
    return false;
  }

  return m_database.commit();
}

bool DatabaseWorker::prekeyExistsRaw(const QByteArray& prekeyId) {
  if (!m_database.isOpen() && !m_database.open()) {
    qCritical() << "Worker: DB not open and cannot reopen!";
    return false;
  }

  QSqlQuery q(m_database);
  q.prepare("SELECT COUNT(*) FROM prekeys WHERE id = ?");
  q.bindValue(0, prekeyId);

  if (!q.exec() || !q.next()) {
    qCritical() << "Worker prekeyExistsRaw failed:" << q.lastError().text();
    return false;
  }

  return q.value(0).toInt() > 0;
}

pqdh::KeyPairs DatabaseWorker::loadPrekeyRaw(const QByteArray& prekeyId) {
  pqdh::KeyPairs keypairs;

  if (!m_database.isOpen() && !m_database.open()) {
    qCritical() << "Worker: DB not open and cannot reopen!";
    return keypairs;
  }

  QSqlQuery q(m_database);
  q.prepare("SELECT * FROM prekeys WHERE id = ?");
  q.bindValue(0, prekeyId);

  if (!q.exec()) {
    qCritical() << "Worker loadPrekeyRaw failed:" << q.lastError().text();
    return keypairs;
  }

  if (q.next()) {
    keypairs.dh_pub = qutils::to_vector(q.value("dh_pub").toByteArray());
    keypairs.pq_pk = qutils::to_vector(q.value("pq_pub").toByteArray());
    keypairs.dh_priv = qutils::to_vector(q.value("dh_priv").toByteArray());
    keypairs.pq_sk = qutils::to_vector(q.value("pq_priv").toByteArray());
  }

  return keypairs;
}

dr::DoubleRatchet DatabaseWorker::loadDoubleRatchetValue(
    const QString& serverId, const QString& pubKeyFingerprint) {
  return m_contactStore.loadDoubleRatchet(serverId, pubKeyFingerprint);
}

void DatabaseWorker::saveDoubleRatchetValue(
    const QString& serverId, const QString& pubKeyFingerprint,
    const dr::DoubleRatchet& doubleRatchet) {
  m_contactStore.saveDoubleRatchet(serverId, pubKeyFingerprint, doubleRatchet);
}

QByteArray DatabaseWorker::loadMessageContentValue(const QString& serverId,
                                                   const QString& contactPubKey,
                                                   quint64 messageId) {
  return m_messageStore.loadMessageContentValue(serverId, contactPubKey,
                                                messageId);
}

bool DatabaseWorker::saveMessageContentValue(const QString& serverId,
                                             const QString& contactPubKey,
                                             quint64 id,
                                             const QByteArray& content) {
  return m_messageStore.saveMessageContentValue(serverId, contactPubKey, id,
                                                content);
}

quint64 DatabaseWorker::insertTypedMessageValue(
    quint64 mid, const QString& serverId, const QString& toPubKey,
    const QString& text, MessageModel::MESSAGE_TYPE type, quint64 timestamp,
    bool isOwn, bool isSended, bool isReaded, bool isReceived,
    const QByteArray& content, const ReplyInfo& reply) {
  return m_messageStore.insertTypedMessageValue(
      mid, serverId, toPubKey, text, type, timestamp, isOwn, isSended, isReaded,
      isReceived, content, reply);
}

QStringList DatabaseWorker::loadKnownServerAddressesValue() {
  return m_contactStore.loadKnownServerAddresses();
}

#include "apppaths.h"

bool DatabaseWorker::storeAnonymousContactSessionValue(
    const QString& serverId, const QString& contactPubKey,
    const QString& doubleRatchetJson) {
  return m_contactStore.storeAnonymousContactSession(serverId, contactPubKey,
                                                     doubleRatchetJson);
}

bool DatabaseWorker::contactExistsValue(const QString& serverId,
                                        const QString& pubKeyFingerprint) {
  return m_contactStore.contactExists(serverId, pubKeyFingerprint);
}

DatabaseManager::DatabaseManager(QObject* parent,
                                 AvatarProvider* avatarProvider)
    : QObject(parent), m_isEncrypted(false), m_avatarProvider(avatarProvider) {
  qRegisterMetaType<Contact>("Contact");
  qRegisterMetaType<QList<Contact>>("QList<Contact>");
  qRegisterMetaType<Message>("Message");
  qRegisterMetaType<QList<Message>>("QList<Message>");

  m_connectionName = generateConnectionName();

  m_workerThread = new QThread(this);
  m_worker = new DatabaseWorker(apppaths::databasePath());

  // Move the worker object to a new thread
  m_worker->moveToThread(m_workerThread);

  // Connect signals
  connect(this, &DatabaseManager::databasePathResolved, m_worker,
          &DatabaseWorker::setDatabasePath);
  connect(this, &DatabaseManager::databaseOpened, m_worker,
          &DatabaseWorker::initDatabase);
  connect(this, &DatabaseManager::sigAddMessage, m_worker,
          &DatabaseWorker::addMessage);
  connect(this, &DatabaseManager::sigMarkMessagesAsReaded, m_worker,
          &DatabaseWorker::markMessagesAsReaded);
  connect(this, &DatabaseManager::sigUpdateMessageSendedStatus, m_worker,
          &DatabaseWorker::updateMessageSendedStatus);
  connect(this, &DatabaseManager::sigUpdateMessageDeliveredStatus, m_worker,
          &DatabaseWorker::updateMessageDeliveredStatus);
  connect(this, &DatabaseManager::sigUpdateCallDuration, m_worker,
          &DatabaseWorker::updateCallDuration);
  connect(this, &DatabaseManager::sigLoadContacts, m_worker,
          &DatabaseWorker::loadContacts);
  connect(this, &DatabaseManager::sigLoadMessages, m_worker,
          &DatabaseWorker::loadMessages);
  connect(this, &DatabaseManager::sigDeleteContact, m_worker,
          &DatabaseWorker::onContactRemove);
  connect(this, &DatabaseManager::sigUpdateContact, m_worker,
          &DatabaseWorker::updateContactRecord);
  connect(this, &DatabaseManager::passwordChangedWithNewPassword, m_worker,
          &DatabaseWorker::reInitDatabase);

  connect(m_worker, &DatabaseWorker::contactsLoaded, this,
          [this](const QList<Contact>& contacts) {
            QList<Contact> enrichedContacts = contacts;
            enrichContactsWithAvatars(&enrichedContacts);
            emit contactsLoaded(enrichedContacts);
          });
  connect(m_worker, &DatabaseWorker::messagesLoaded, this,
          &DatabaseManager::messagesLoaded);

  connect(&MainSignals::instance(), &MainSignals::messageSended, m_worker,
          &DatabaseWorker::onMessageSended);
  connect(&MainSignals::instance(), &MainSignals::messageStatusDelivered,
          m_worker, &DatabaseWorker::onMessageStatusDelivered);
  connect(&MainSignals::instance(), &MainSignals::messageStatusSended, m_worker,
          &DatabaseWorker::onMessageStatusSended);
  connect(&MainSignals::instance(), &MainSignals::lastOnlineChanged, m_worker,
          &DatabaseWorker::onLastOnlineChanged);
  connect(&MainSignals::instance(), &MainSignals::timerTicked, m_worker,
          &DatabaseWorker::onTimerTicked);
  connect(&MainSignals::instance(), &MainSignals::e2eTextMessageReceived,
          m_worker, &DatabaseWorker::onE2eTextMessageReceived);
  connect(&MainSignals::instance(), &MainSignals::allMessagesReaded, m_worker,
          &DatabaseWorker::onAllMessagesReaded);
  connect(&MainSignals::instance(), &MainSignals::allOwnMessagesReaded,
          m_worker, &DatabaseWorker::onAllOwnMessagesReaded);
  connect(&MainSignals::instance(), &MainSignals::settingSetBool, m_worker,
          &DatabaseWorker::onSettingSetBool);
  connect(&MainSignals::instance(), &MainSignals::settingSetText, m_worker,
          &DatabaseWorker::onSettingSetText);
  connect(&MainSignals::instance(), &MainSignals::settingSetBlob, m_worker,
          &DatabaseWorker::onSettingSetBlob);

  connect(&MainSignals::instance(), &MainSignals::avatarDeleted, m_worker,
          &DatabaseWorker::onAvatarDeleted);
  connect(&MainSignals::instance(), &MainSignals::messageDelete, m_worker,
          &DatabaseWorker::onMessageDelete);
  connect(&MainSignals::instance(), &MainSignals::localMessageDelete, m_worker,
          &DatabaseWorker::onMessageDelete);
  connect(&MainSignals::instance(), &MainSignals::messageDeleteReceived,
          m_worker, &DatabaseWorker::onMessageDelete);
  connect(&MainSignals::instance(), &MainSignals::contactRemove, m_worker,
          &DatabaseWorker::onContactRemove);
  connect(&MainSignals::instance(), &MainSignals::sessionRemoveReceived,
          m_worker, &DatabaseWorker::onContactRemove);
  connect(&MainSignals::instance(), &MainSignals::identityRevocationReceived,
          m_worker, &DatabaseWorker::onIdentityRevoked);
  connect(&MainSignals::instance(), &MainSignals::doubleRatchetUpdate, m_worker,
          &DatabaseWorker::onDoubleRatchetUpdate);
  connect(&MainSignals::instance(), &MainSignals::updateMessageDeliveredStatus,
          m_worker, &DatabaseWorker::onUpdateMessageDeliveredStatus);
  connect(&MainSignals::instance(), &MainSignals::messageReaded, m_worker,
          &DatabaseWorker::onMessageReaded);
  connect(&MainSignals::instance(), &MainSignals::clearHistory, m_worker,
          &DatabaseWorker::onClearHistory);
  connect(&MainSignals::instance(), &MainSignals::clearHistoryReceived,
          m_worker, &DatabaseWorker::onClearHistory);
  connect(&MainSignals::instance(), &MainSignals::setTimer, m_worker,
          &DatabaseWorker::onSetTimer);
  connect(&MainSignals::instance(), &MainSignals::setTimerReceived, m_worker,
          &DatabaseWorker::onSetTimer);

  connect(&MainSignals::instance(), &MainSignals::secondTimerTicked, m_worker,
          &DatabaseWorker::onSecondTimerTicked);

  // connect(&MainSignals::instance(), &MainSignals::addInfoMessage, m_worker,
  // &DatabaseWorker::onAddInfoMessage);
  connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

  m_workerThread->start();
}

DatabaseManager::~DatabaseManager() {
  closeDatabase();

  if (m_workerThread != nullptr) {
    m_workerThread->quit();
    m_workerThread->wait();
  }
}

QString DatabaseManager::generateConnectionName() const {
  return QStringLiteral("db_") +
         QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool DatabaseManager::openDatabase(const QString& path,
                                   const QString& password) {
  QMutexLocker locker(&mutex_);

  if (m_database.isOpen()) {
    closeDatabase();
  }

  m_databasePath = path;
  emit databasePathChanged();

  const QFileInfo fileInfo(path);
  QDir dir = fileInfo.dir();
  if (!dir.exists()) {
    dir.mkpath(QStringLiteral("."));
  }

  m_database = QSqlDatabase::addDatabase(
      "QSQLITE", m_connectionName);  // switch to QSQLCIPHER in the future
  m_database.setDatabaseName(path);

  if (!m_database.open()) {
    setLastError(m_database.lastError().text());
    return false;
  }

  if (!password.isEmpty()) {
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("PRAGMA cipher_compatibility = 4"));
    query.exec(QStringLiteral("PRAGMA kdf_iter = %1").arg(PBKDF2_ITERATIONS));
    query.exec(QStringLiteral("PRAGMA key = '%1'").arg(password));

    if (!query.exec(QStringLiteral("SELECT count(*) FROM sqlite_master"))) {
      setLastError(tr("Invalid password or corrupted database"));
      m_database.close();
      QSqlDatabase::removeDatabase(m_connectionName);
      return false;
    }

    m_isEncrypted = true;
  } else {
    m_isEncrypted = false;
  }

  emit isEncryptedChanged();
  emit isOpenChanged();

  ensureTablesExist();

  m_password = password;
  emit databasePathResolved(m_databasePath);
  emit databaseOpened(password);
  return true;
}

void DatabaseWorker::loadContacts() {
  QList<Contact> contacts = m_contactStore.loadContactsBase();
  // Last-activity timestamp per contact, aligned index-for-index with
  // `contacts`; used below to order the list newest-first.
  QList<quint64> sortKeys;
  sortKeys.reserve(contacts.size());
  for (Contact& contact : contacts) {
    const Message lastMessage = m_messageStore.loadLastMessage(
        contact.server, contact.pubkeyFingerprint);
    quint64 sortKey = lastMessage.t;
    contact.lastMessage = lastMessage.text;
    if ((contact.lastMessage.count('\n') == 1 ||
         contact.lastMessage.count('\n') == 0) &&
        !contact.lastMessage.isEmpty()) {
      contact.lastMessage += '\n';
    }

    contact.time = lastMessage.time;
    if (!lastMessage.isReaded && !lastMessage.isOwn &&
        !lastMessage.text.isEmpty()) {
      contact.unread =
          m_messageStore.unreadCount(contact.server, contact.pubkeyFingerprint);
    }

    contact.sended = lastMessage.isSended;
    contact.readed = lastMessage.isReaded;

    // Check pending file uploads — may be newer than the last sent message
    QSqlQuery pq(m_database);
    pq.prepare(
        QStringLiteral("SELECT preview_text, timestamp FROM pending_file_uploads "
                       "WHERE server_id = ? AND contact_pub_key = ? "
                       "ORDER BY timestamp DESC LIMIT 1"));
    pq.bindValue(0, contact.server);
    pq.bindValue(1, contact.pubkeyFingerprint);
    if (pq.exec() && pq.next()) {
      const quint64 pendingTs = pq.value(1).toULongLong();
      if (pendingTs >= lastMessage.t) {
        QString pendingPreview = pq.value(0).toString();
        if (!pendingPreview.contains('\n')) {
          pendingPreview += '\n';
        }
        contact.lastMessage = pendingPreview;
        contact.time = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(pendingTs))
                           .toString(QStringLiteral("HH:mm"));
        contact.sended = false;
        contact.readed = false;
        sortKey = pendingTs;
      }
    }

    sortKeys.append(sortKey);
  }

  // Order contacts newest-first by their last-activity timestamp so the list
  // matches the in-session move-to-top behaviour and survives restarts.
  // Contacts with no messages (sortKey == 0) sink to the bottom; stable_sort
  // preserves their relative order.
  QList<int> order;
  order.reserve(contacts.size());
  for (int i = 0; i < contacts.size(); ++i) {
    order.append(i);
  }
  std::stable_sort(order.begin(), order.end(),
                   [&sortKeys](int a, int b) { return sortKeys[a] > sortKeys[b]; });

  QList<Contact> ordered;
  ordered.reserve(contacts.size());
  for (const int i : order) {
    ordered.append(contacts[i]);
  }

  emit contactsLoaded(ordered);
}

void DatabaseWorker::loadMessages(const QString& serverId,
                                  const QString& contactPubKey) {
  emit messagesLoaded(serverId, contactPubKey,
                      m_messageStore.loadMessages(serverId, contactPubKey));
}

void DatabaseWorker::updateContactRecord(const QString& serverId,
                                         const QString& contactPubKey,
                                         const QString& firstName,
                                         const QString& lastName,
                                         const QString& aboutMe,
                                         const QString& nameStyle,
                                         const QByteArray& avatarBlob) {
  m_contactStore.updateContactRecord(serverId, contactPubKey, firstName,
                                     lastName, aboutMe, nameStyle, avatarBlob);
}

void DatabaseManager::requestContactsLoad() { emit sigLoadContacts(); }

void DatabaseManager::requestMessagesLoad(const QString& serverId,
                                          const QString& contactPubKey) {
  emit sigLoadMessages(serverId, contactPubKey);
}

void DatabaseManager::requestContactRemoval(const QString& serverId,
                                            const QString& contactPubKey) {
  emit sigDeleteContact(serverId, contactPubKey);
}

void DatabaseManager::requestContactUpdate(const QString& serverId,
                                           const QString& contactPubKey,
                                           const QString& firstName,
                                           const QString& lastName,
                                           const QString& aboutMe,
                                           const QString& nameStyle,
                                           const QByteArray& avatarBlob) {
  emit sigUpdateContact(serverId, contactPubKey, firstName, lastName, aboutMe,
                        nameStyle, avatarBlob);
}

bool DatabaseManager::closeDatabase() {
  QMutexLocker locker(&mutex_);

  if (m_database.isOpen()) {
    m_database.close();
  }

  QSqlDatabase::removeDatabase(m_connectionName);
  m_connectionName = generateConnectionName();

  m_doubleRatchetCache.clear();

  emit isOpenChanged();
  emit databaseClosed();

  return true;
}

bool DatabaseManager::changePassword(const QString& newPassword) {
  QMutexLocker locker(&mutex_);

  if (!m_database.isOpen()) {
    setLastError(tr("Database is not open"));
    return false;
  }

  QSqlQuery query(m_database);

  query.exec(QStringLiteral("PRAGMA kdf_iter = %1").arg(PBKDF2_ITERATIONS));
  if (!query.exec(QStringLiteral("PRAGMA rekey = '%1'").arg(newPassword))) {
    setLastError(query.lastError().text());
    return false;
  }
  m_isEncrypted = true;

  emit isEncryptedChanged();
  emit passwordChanged();
  emit passwordChangedWithNewPassword(newPassword);

  return true;
}

bool DatabaseManager::execute(const QString& query,
                              const QVariantList& params) {
  qDebug() << "[DatabaseManager] [execute] Executing query:" << query;
  return runWorkerBlocking(m_worker, [this, query, params]() {
    return m_worker->executeQuery(query, params);
  });
}

bool DatabaseManager::wipeAllData() {
  qDebug() << "[DatabaseManager] [wipeAllData] Erasing all account data";
  const bool wiped = runWorkerBlocking(
      m_worker, [this]() { return m_worker->wipeAllData(); });
  if (!wiped) {
    return false;
  }

  // wipeAllData() deletes every row, including the settings tables' seeded
  // defaults (e.g. ignoreSslErrors=1) - without this, they silently read back
  // as each call site's hardcoded fallback (often false/off) until the app is
  // fully restarted and ensureSchema() reseeds them on the next open.
  ensureTablesExist();
  emit defaultsReseeded();
  return true;
}

QVariantList DatabaseManager::select(const QString& query,
                                     const QVariantList& params) {
  qDebug() << "[DatabaseManager] [select] Executing select query:" << query
           << "with params:" << params;
  return runWorkerBlocking(m_worker, [this, query, params]() {
    return m_worker->selectRows(query, params);
  });
}

QVariant DatabaseManager::selectOne(const QString& query,
                                    const QVariantList& params) {
  QVariantList results = select(query, params);
  if (results.isEmpty()) {
    return QVariant();
  }
  return results.first();
}

bool DatabaseManager::beginTransaction() {
  return runWorkerBlocking(m_worker,
                           [this]() { return m_worker->beginTransaction(); });
}

bool DatabaseManager::commitTransaction() {
  return runWorkerBlocking(m_worker,
                           [this]() { return m_worker->commitTransaction(); });
}

bool DatabaseManager::isOpen() const { return m_database.isOpen(); }

bool DatabaseManager::isEncrypted() const { return m_isEncrypted; }

QString DatabaseManager::lastError() const { return m_lastError; }

QString DatabaseManager::databasePath() const { return m_databasePath; }

void DatabaseManager::setLastError(const QString& error) {
  m_lastError = error;
  emit lastErrorChanged();
  emit errorOccurred(error);
  qWarning() << "[DatabaseManager] Error:" << error;
}

bool DatabaseManager::configureCipher() {
  QMutexLocker locker(&mutex_);

  QSqlQuery query(m_database);

  query.exec(QStringLiteral("PRAGMA cipher_page_size = 4096"));
  query.exec(QStringLiteral("PRAGMA kdf_iter = %1").arg(PBKDF2_ITERATIONS));
  query.exec(QStringLiteral("PRAGMA cipher_hmac_algorithm = HMAC_SHA512"));
  query.exec(
      QStringLiteral("PRAGMA cipher_kdf_algorithm = PBKDF2_HMAC_SHA512"));

  return true;
}

void DatabaseManager::ensureTablesExist() {
  DatabaseSchemaService schemaService;
  QString lastError;
  if (!schemaService.ensureSchema(&m_database, &lastError) &&
      !lastError.isEmpty()) {
    setLastError(lastError);
  }
}

void DatabaseManager::enrichContactsWithAvatars(QList<Contact>* contacts) {
  if (contacts == nullptr || m_avatarProvider == nullptr) {
    return;
  }

  const QString timestamp = QString::number(QDateTime::currentSecsSinceEpoch());
  for (Contact& contact : *contacts) {
    const QString avatarId = AccountManager::contactAvatarId(
        contact.server, contact.pubkeyFingerprint);
    if (m_avatarProvider->hasAvatar(avatarId)) {
      contact.avatarSource = QStringLiteral("image://avatars/") + avatarId +
                             QStringLiteral("?t=") + timestamp;
    } else if (!contact.avatarBlob.isEmpty()) {
      m_avatarProvider->addAvatar(avatarId, contact.avatarBlob);
      contact.avatarSource = QStringLiteral("image://avatars/") + avatarId +
                             QStringLiteral("?t=") + timestamp;
    }
  }
}

dr::DoubleRatchet DatabaseManager::getDoubleRatchet(
    const QString& serverId, const QString& pubKeyFingerprint) {
  const QString cacheKey = identityKey(serverId, pubKeyFingerprint);
  if (m_doubleRatchetCache.contains(cacheKey)) {
    return m_doubleRatchetCache.value(cacheKey);
  }

  qDebug() << "[DatabaseManager] [getDoubleRatchet] Loading DoubleRatchet for "
              "contact"
           << pubKeyFingerprint;
  const dr::DoubleRatchet doubleRatchet =
      runWorkerBlocking(m_worker, [this, serverId, pubKeyFingerprint]() {
        return m_worker->loadDoubleRatchetValue(serverId, pubKeyFingerprint);
      });

  m_doubleRatchetCache.insert(cacheKey, doubleRatchet);
  return doubleRatchet;
}

void DatabaseManager::setDoubleRatchet(const QString& serverId,
                                       const QString& pubKeyFingerprint,
                                       const dr::DoubleRatchet& doubleRatchet) {
  const QString cacheKey = identityKey(serverId, pubKeyFingerprint);
  m_doubleRatchetCache.insert(cacheKey, doubleRatchet);

  runWorkerBlockingVoid(m_worker,
                        [this, serverId, pubKeyFingerprint, doubleRatchet]() {
                          m_worker->saveDoubleRatchetValue(
                              serverId, pubKeyFingerprint, doubleRatchet);
                        });

  MainSignals::instance().emitDoubleRatchetUpdate(serverId, pubKeyFingerprint,
                                                  doubleRatchet.to_json());
}

Message DatabaseManager::getLastMessage(const QString& serverId,
                                        const QString& contactPubKey) {
  qDebug()
      << "[DatabaseManager] [getLastMessage] Loading last message from contact"
      << contactPubKey;
  Message message;

  const QVariantMap row = selectOne(
                              "SELECT * FROM messages WHERE serverAddress = ? "
                              "AND fromPubKey = ? ORDER BY t DESC LIMIT 1",
                              {serverId, contactPubKey})
                              .toMap();
  if (row.isEmpty()) {
    return message;
  }

  const MessageModel::MESSAGE_TYPE msgType =
      static_cast<MessageModel::MESSAGE_TYPE>(row.value("type").toUInt());
  if (msgType == MessageModel::MESSAGE_TYPE::TEXT) {
    message.text = row.value("text").toString();
    message.isMsg = true;
  } else if (msgType == MessageModel::MESSAGE_TYPE::CALL) {
    message.text = row.value("text").toString();
    message.isMsg = true;
    message.isCall = true;
  } else if (msgType == MessageModel::MESSAGE_TYPE::INFO) {
    message.text = row.value("text").toString();
    message.isInfo = true;
  } else if (msgType == MessageModel::MESSAGE_TYPE::FILE ||
             msgType == MessageModel::MESSAGE_TYPE::AUDIO) {
    message.text = row.value("text").toString();
    message.isMsg = true;
    message.isFile = true;
    message.isAudio = (msgType == MessageModel::MESSAGE_TYPE::AUDIO);
    filemessage::deserialize(row.value("content").toByteArray(),
                             &message.fileData);
    restoreAudioWaveformIfNeeded(&message.fileData);
  }

  const uint64_t timestamp = row.value("t").toULongLong();
  message.time = QDateTime::fromSecsSinceEpoch(timestamp).toString("HH:mm");
  message.isOwn = row.value("isOwn").toBool();
  message.isSended = row.value("isSended").toBool();
  message.isReaded = row.value("isReaded").toBool();
  message.isReceived = row.value("isReceived").toBool();
  message.id = row.value("id").toULongLong();
  message.mid = message.id;

  return message;
}

QVariantMap DatabaseManager::getLastMessageMap(const QString& serverId,
                                               const QString& contactPubKey) {
  QVariantMap map;
  Message message = getLastMessage(serverId, contactPubKey);

  map["text"] = message.text;
  map["time"] = message.time;
  map["isOwn"] = message.isOwn;
  map["isMsg"] = message.isMsg;
  map["isDate"] = message.isDate;
  map["isCall"] = message.isCall;
  map["isSended"] = message.isSended;
  map["isReaded"] = message.isReaded;
  map["isReceived"] = message.isReceived;

  return map;
}

bool DatabaseManager::contactExists(const QString& serverId,
                                    const QString& pubKeyFingerprint) {
  qDebug() << "[DatabaseManager] [contactExists] Checking existence of contact"
           << pubKeyFingerprint << "on server" << serverId;
  return runWorkerBlocking(m_worker, [this, serverId, pubKeyFingerprint]() {
    return m_worker->contactExistsValue(serverId, pubKeyFingerprint);
  });
}

bool DatabaseManager::prekeyExists(const QByteArray& prekeyId) {
  return runWorkerBlocking(m_worker, [this, prekeyId]() {
    return m_worker->prekeyExistsRaw(prekeyId);
  });
}

bool DatabaseManager::prekeyExists(const QString& prekeyId) {
  return prekeyExists(QByteArray::fromHex(prekeyId.toUtf8()));
}

bool DatabaseManager::prekeyExists(const std::vector<uint8_t>& prekeyId) {
  return prekeyExists(qutils::from_vector(prekeyId));
}

pqdh::KeyPairs DatabaseManager::getPrekey(const QByteArray& prekeyId) {
  return runWorkerBlocking(m_worker, [this, prekeyId]() {
    return m_worker->loadPrekeyRaw(prekeyId);
  });
}

pqdh::KeyPairs DatabaseManager::getPrekey(const QString& prekeyId) {
  return getPrekey(QByteArray::fromHex(prekeyId.toUtf8()));
}

pqdh::KeyPairs DatabaseManager::getPrekey(
    const std::vector<uint8_t>& prekeyId) {
  return getPrekey(qutils::from_vector(prekeyId));
}

quint64 DatabaseManager::addTypedMessage(
    const quint64 mid, const QString& serverId, const QString& toPubKey,
    const QString& text, MessageModel::MESSAGE_TYPE type, quint64 timestamp,
    bool isOwn, bool isSended, bool isReaded, bool isReceived,
    const QByteArray& content, const ReplyInfo& reply) {
  return runWorkerBlocking(m_worker, [this, mid, serverId, toPubKey, text, type,
                                      timestamp, isOwn, isSended, isReaded,
                                      isReceived, content, reply]() {
    return m_worker->insertTypedMessageValue(
        mid, serverId, toPubKey, text, type, timestamp, isOwn, isSended,
        isReaded, isReceived, content, reply);
  });
}

quint64 DatabaseManager::addMessage(const quint64 mid, const QString& serverId,
                                    const QString& toPubKey,
                                    const QString& text, quint64 timestamp,
                                    bool isOwn, bool isMsg, bool isSended,
                                    bool isReaded, bool isReceived,
                                    bool isCall) {
  Q_UNUSED(isMsg);
  return addTypedMessage(mid, serverId, toPubKey, text,
                         isCall ? MessageModel::MESSAGE_TYPE::CALL
                                : MessageModel::MESSAGE_TYPE::TEXT,
                         timestamp, isOwn, isSended, isReaded, isReceived,
                         QByteArray());
}

quint64 DatabaseManager::addInfoMessage(const QString& serverId,
                                        const QString& toPubKey,
                                        const QString& text) {
  const Message lastMessage = getLastMessage(serverId, toPubKey);
  const quint64 messageId = lastMessage.id + 1;
  const quint64 result = addTypedMessage(
      messageId, serverId, toPubKey, text, MessageModel::MESSAGE_TYPE::INFO,
      static_cast<quint64>(QDateTime::currentDateTimeUtc().toSecsSinceEpoch()),
      true, true, true, true, QByteArray());
  return result == static_cast<quint64>(-1) ? static_cast<quint64>(-1)
                                            : messageId;
}

void DatabaseManager::NEW_THREAD_addMessage(
    const quint64 mid, const QString& serverId, const QString& toPubKey,
    const QString& text, quint64 timestamp, bool isOwn, bool isMsg,
    bool isSended, bool isReaded, bool isReceived, bool isCall) {
  // Just emit the signal. Control returns immediately.
  // Qt will copy the arguments and queue them on the other thread.
  emit sigAddMessage(mid, serverId, toPubKey, text, timestamp, isOwn, isMsg,
                     isSended, isReaded, isReceived, isCall);
}

QByteArray DatabaseManager::getMessageContent(const QString& serverId,
                                              const QString& contactPubKey,
                                              const quint64 messageId) {
  return runWorkerBlocking(m_worker,
                           [this, serverId, contactPubKey, messageId]() {
                             return m_worker->loadMessageContentValue(
                                 serverId, contactPubKey, messageId);
                           });
}

bool DatabaseManager::updateMessageContent(const QString& serverId,
                                           const QString& contactPubKey,
                                           const quint64 id,
                                           const QByteArray& content) {
  return runWorkerBlocking(m_worker,
                           [this, serverId, contactPubKey, id, content]() {
                             return m_worker->saveMessageContentValue(
                                 serverId, contactPubKey, id, content);
                           });
}

int DatabaseManager::getUnreadedCount(const QString& serverId,
                                      const QString& contactPubKey) {
  qDebug() << "[DatabaseManager] [getUnreadedCount] Getting unreaded count for "
              "contact"
           << contactPubKey;
  return selectOne(
             "SELECT COUNT(*) AS unreadCount FROM messages WHERE serverAddress "
             "= ? AND fromPubKey = ? AND isOwn = 0 AND isReaded = 0",
             {serverId, contactPubKey})
      .toMap()
      .value("unreadCount")
      .toInt();
}

int DatabaseManager::getUnreadCount(const QString& serverId,
                                    const QString& contactPubKey) {
  return getUnreadedCount(serverId, contactPubKey);
}

void DatabaseManager::updateMessageSendedStatus(const QString& serverId,
                                                const QString& contactPubKey,
                                                const quint64 id) {
  qDebug() << "[DatabaseManager] [updateMessageSendedStatus] Updating message "
              "sended status for message ID"
           << id;
  emit sigUpdateMessageSendedStatus(serverId, contactPubKey, id);
}

void DatabaseManager::updateMessageSentStatus(const QString& serverId,
                                              const QString& contactPubKey,
                                              const quint64 id) {
  updateMessageSendedStatus(serverId, contactPubKey, id);
}

void DatabaseManager::updateMessageDeliveredStatus(const QString& serverId,
                                                   const QString& contactPubKey,
                                                   const quint64 id) {
  qDebug() << "[DatabaseManager] [updateMessageDeliveredStatus] Updating "
              "message delivered status for message ID"
           << id;
  emit sigUpdateMessageDeliveredStatus(serverId, contactPubKey, id);
}

void DatabaseManager::updateCallDuration(const QString& serverId,
                                         const QString& contactPubKey,
                                         const quint64 id,
                                         quint64 durationSec) {
  emit sigUpdateCallDuration(serverId, contactPubKey, id, durationSec);
}

void DatabaseManager::storePendingFileUpload(
    const QString& serverId, const QString& contactPubKey, quint64 messageId,
    int messageType, const QString& encryptedPath, const QByteArray& fileData,
    quint64 timestamp, const QString& previewText) {
  execute(
      QStringLiteral("INSERT OR REPLACE INTO pending_file_uploads "
                     "(message_id, server_id, contact_pub_key, message_type, "
                     "encrypted_path, file_data, timestamp, preview_text) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"),
      {static_cast<qulonglong>(messageId), serverId, contactPubKey, messageType,
       encryptedPath, fileData, static_cast<qulonglong>(timestamp), previewText});
}

QVariantList DatabaseManager::loadPendingFileUploads(const QString& serverId) {
  return select(
      QStringLiteral("SELECT message_id, server_id, contact_pub_key, "
                     "message_type, encrypted_path, file_data, timestamp, "
                     "preview_text FROM pending_file_uploads "
                     "WHERE server_id = ? ORDER BY timestamp ASC"),
      {serverId});
}

QVariantList DatabaseManager::loadPendingFileUploadsForContact(
    const QString& serverId, const QString& contactPubKey) {
  return select(
      QStringLiteral("SELECT message_id, server_id, contact_pub_key, "
                     "message_type, encrypted_path, file_data, timestamp, "
                     "preview_text FROM pending_file_uploads "
                     "WHERE server_id = ? AND contact_pub_key = ? "
                     "ORDER BY timestamp ASC"),
      {serverId, contactPubKey});
}

QString DatabaseManager::pendingFileUploadEncryptedPath(
    quint64 messageId, const QString& serverId) {
  return selectOne(
             QStringLiteral("SELECT encrypted_path FROM pending_file_uploads "
                            "WHERE message_id = ? AND server_id = ?"),
             {static_cast<qulonglong>(messageId), serverId})
      .toMap()
      .value(QStringLiteral("encrypted_path"))
      .toString();
}

void DatabaseManager::deletePendingFileUpload(quint64 messageId,
                                              const QString& serverId) {
  execute(
      QStringLiteral("DELETE FROM pending_file_uploads "
                     "WHERE message_id = ? AND server_id = ?"),
      {static_cast<qulonglong>(messageId), serverId});
}

QVariantList DatabaseManager::loadPendingAlbumMessages(
    const QString& serverId) {
  return select(
      QStringLiteral(
          "SELECT id, fromPubKey, content, t, replyTo, replyPreview, "
          "replyKind, replyIsOwn FROM messages WHERE serverAddress = ? AND "
          "isOwn = 1 AND isSended = 0 AND type = ? ORDER BY t ASC, id ASC"),
      {serverId, static_cast<int>(MessageModel::MESSAGE_TYPE::ALBUM)});
}

void DatabaseManager::storePendingAlbumUpload(
    quint64 albumMessageId, const QString& serverId,
    const QString& contactPubKey, const QString& caption, quint64 timestamp,
    const ReplyInfo& reply, int totalItems, const QByteArray& itemsJson) {
  execute(
      QStringLiteral(
          "INSERT OR REPLACE INTO pending_album_uploads "
          "(album_message_id, server_id, contact_pub_key, caption, timestamp, "
          "reply_to, reply_preview, reply_kind, reply_is_own, total_items, "
          "items_json) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"),
      {static_cast<qulonglong>(albumMessageId), serverId, contactPubKey, caption,
       static_cast<qulonglong>(timestamp),
       static_cast<qulonglong>(reply.replyTo), reply.preview, reply.kind,
       reply.isOwn ? 1 : 0, totalItems, itemsJson});
}

QVariantList DatabaseManager::loadPendingAlbumUploads(const QString& serverId) {
  return select(
      QStringLiteral(
          "SELECT album_message_id, server_id, contact_pub_key, caption, "
          "timestamp, reply_to, reply_preview, reply_kind, reply_is_own, "
          "total_items, items_json FROM pending_album_uploads "
          "WHERE server_id = ? ORDER BY timestamp ASC"),
      {serverId});
}

void DatabaseManager::deletePendingAlbumUpload(quint64 albumMessageId,
                                               const QString& serverId) {
  execute(
      QStringLiteral("DELETE FROM pending_album_uploads "
                     "WHERE album_message_id = ? AND server_id = ?"),
      {static_cast<qulonglong>(albumMessageId), serverId});
}

QString DatabaseManager::getTimerValueS(int timerValue) {
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
    return QString::number(hoursValue) + " " + (hoursValue == 1 ? hour : hours);
  } else {
    int daysValue = timerValue / 86400;
    return QString::number(daysValue) + " " + (daysValue == 1 ? day : days);
  }
  return "";
}