#include "databaseschemaservice.h"

#include <QPair>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

bool DatabaseSchemaService::ensureSchema(QSqlDatabase* database,
                                         QString* lastError) const {
  if (database == nullptr || !database->isOpen()) {
    if (lastError != nullptr) {
      *lastError = QStringLiteral("Database is not open");
    }
    return false;
  }

  QSqlQuery query(*database);
  const QStringList statements = {
      QStringLiteral("CREATE TABLE IF NOT EXISTS blobvalues (name TEXT PRIMARY "
                     "KEY, value BLOB)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS blobsettings (name TEXT "
                     "PRIMARY KEY, value BLOB)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS textsettings (name TEXT "
                     "PRIMARY KEY, value TEXT)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS intsettings (name TEXT "
                     "PRIMARY KEY, value INTEGER)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS boolsettings (name TEXT "
                     "PRIMARY KEY, value INTEGER)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS contacts (serverAddress TEXT "
                     "NOT NULL, pubKey TEXT NOT NULL, firstName TEXT, lastName "
                     "TEXT, aboutMe TEXT, avatar BLOB, doubleratchet TEXT, "
                     "lastOnline TEXT DEFAULT CURRENT_TIMESTAMP, timerValue "
                     "INTEGER DEFAULT 0, nameStyle TEXT DEFAULT '', revoked "
                     "INTEGER NOT NULL DEFAULT 0, isPending INTEGER NOT NULL "
                     "DEFAULT 0, memberEnrolled INTEGER NOT NULL DEFAULT 0, "
                     "PRIMARY KEY "
                     "(serverAddress, pubKey))"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS prekeys (id BLOB PRIMARY KEY, "
                     "dh_pub BLOB, pq_pub BLOB, ed25519_pub BLOB, signature "
                     "BLOB, dh_priv BLOB, pq_priv BLOB)"),
      QStringLiteral(
          "CREATE TABLE IF NOT EXISTS messages (id INTEGER NOT NULL, "
          "serverAddress TEXT NOT NULL DEFAULT '', fromPubKey TEXT NOT NULL "
          "DEFAULT '', type INTEGER, text TEXT, t INTEGER, isOwn INTEGER, "
          "isSended INTEGER, isReaded INTEGER, isReceived INTEGER, content "
          "BLOB, receive_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP, readTime "
          "INTEGER DEFAULT 0, timerValue INTEGER DEFAULT 0, callDurationSec "
          "INTEGER NOT NULL DEFAULT 0, replyTo INTEGER NOT NULL DEFAULT 0, "
          "replyPreview TEXT NOT NULL DEFAULT '', replyKind TEXT NOT NULL "
          "DEFAULT '', replyIsOwn INTEGER NOT NULL DEFAULT 0, PRIMARY KEY "
          "(serverAddress, fromPubKey, id))"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_contacts_server_pubkey ON "
                     "contacts(serverAddress, pubKey)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_server_pubkey_t "
                     "ON messages(serverAddress, fromPubKey, t)"),
      QStringLiteral("INSERT OR IGNORE INTO textsettings (name, value) VALUES "
                     "('firstName', 'Anonymous')"),
      QStringLiteral("INSERT OR IGNORE INTO textsettings (name, value) VALUES "
                     "('lastName', '')"),
      QStringLiteral("INSERT OR IGNORE INTO textsettings (name, value) VALUES "
                     "('aboutMe', '')"),
      QStringLiteral("INSERT OR IGNORE INTO textsettings (name, value) VALUES "
                     "('nameStyle', '')"),
      QStringLiteral("INSERT OR IGNORE INTO textsettings (name, value) VALUES "
                     "('serverAddress', 'localhost:443')"),
      QStringLiteral("INSERT OR IGNORE INTO boolsettings (name, value) VALUES "
                     "('startWithSystem', 0)"),
      QStringLiteral("INSERT OR IGNORE INTO boolsettings (name, value) VALUES "
                     "('notificationsEnabled', 1)"),
      QStringLiteral("INSERT OR IGNORE INTO boolsettings (name, value) VALUES "
                     "('soundsEnabled', 1)"),
      QStringLiteral("INSERT OR IGNORE INTO boolsettings (name, value) VALUES "
                     "('soundFromActiveChatEnabled', 1)"),
      QStringLiteral("INSERT OR IGNORE INTO boolsettings (name, value) VALUES "
                     "('callSoundsEnabled', 1)"),
      QStringLiteral("INSERT OR IGNORE INTO boolsettings (name, value) VALUES "
                     "('callVibrationEnabled', 1)"),
      QStringLiteral("INSERT OR IGNORE INTO boolsettings (name, value) VALUES "
                     "('autoDownloadMedia', 1)"),
      QStringLiteral("INSERT OR IGNORE INTO boolsettings (name, value) VALUES "
                     "('autoDownloadFiles', 0)"),
      QStringLiteral("INSERT OR IGNORE INTO boolsettings (name, value) VALUES "
                     "('autoDownloadVoice', 1)"),
      QStringLiteral("INSERT OR IGNORE INTO boolsettings (name, value) VALUES "
                     "('ignoreSslErrors', 1)"),
      QStringLiteral("INSERT OR IGNORE INTO blobsettings (name, value) VALUES "
                     "('avatar', zeroblob(0))"),
      QStringLiteral(
          "CREATE TABLE IF NOT EXISTS pending_file_uploads ("
          "message_id INTEGER NOT NULL, "
          "server_id TEXT NOT NULL, "
          "contact_pub_key TEXT NOT NULL, "
          "message_type INTEGER NOT NULL, "
          "encrypted_path TEXT NOT NULL, "
          "file_data BLOB NOT NULL, "
          "timestamp INTEGER NOT NULL, "
          "preview_text TEXT NOT NULL DEFAULT '', "
          "PRIMARY KEY (message_id, server_id))"),
      // Albums composed while offline: the whole album (encrypted blobs still to
      // upload + assembly metadata) is stored as one row and finished on the
      // next reconnect (retryPendingAlbumUploads). items_json is a JSON array of
      // {index, message_id, encrypted_path, file_data} per item.
      QStringLiteral(
          "CREATE TABLE IF NOT EXISTS pending_album_uploads ("
          "album_message_id INTEGER NOT NULL, "
          "server_id TEXT NOT NULL, "
          "contact_pub_key TEXT NOT NULL, "
          "caption TEXT NOT NULL DEFAULT '', "
          "timestamp INTEGER NOT NULL, "
          "reply_to INTEGER NOT NULL DEFAULT 0, "
          "reply_preview TEXT NOT NULL DEFAULT '', "
          "reply_kind TEXT NOT NULL DEFAULT '', "
          "reply_is_own INTEGER NOT NULL DEFAULT 0, "
          "total_items INTEGER NOT NULL, "
          "items_json BLOB NOT NULL, "
          "PRIMARY KEY (album_message_id, server_id))")};

  for (const QString& statement : statements) {
    if (!query.exec(statement)) {
      if (lastError != nullptr) {
        *lastError = query.lastError().text();
      }
      return false;
    }
  }

  // Migration for databases created before the `revoked` column existed. SQLite
  // has no "ADD COLUMN IF NOT EXISTS", so probe the table first and only ALTER
  // when the column is missing (the CREATE above already includes it on fresh
  // databases).
  {
    bool hasRevoked = false;
    QSqlQuery pragma(*database);
    if (pragma.exec(QStringLiteral("PRAGMA table_info(contacts)"))) {
      while (pragma.next()) {
        if (pragma.value(1).toString() == QStringLiteral("revoked")) {
          hasRevoked = true;
          break;
        }
      }
    }
    if (!hasRevoked) {
      QSqlQuery alter(*database);
      if (!alter.exec(QStringLiteral(
              "ALTER TABLE contacts ADD COLUMN revoked INTEGER NOT NULL "
              "DEFAULT 0"))) {
        if (lastError != nullptr) {
          *lastError = alter.lastError().text();
        }
        return false;
      }
    }
  }

  // Migration for databases created before the `isPending` column existed.
  // Pending rows hold a Double Ratchet session created by an incoming
  // session_request that has not yet proven knowledge of our invite secret
  // (first successful decrypt); they are invisible to the contacts list.
  {
    bool hasIsPending = false;
    QSqlQuery pragma(*database);
    if (pragma.exec(QStringLiteral("PRAGMA table_info(contacts)"))) {
      while (pragma.next()) {
        if (pragma.value(1).toString() == QStringLiteral("isPending")) {
          hasIsPending = true;
          break;
        }
      }
    }
    if (!hasIsPending) {
      QSqlQuery alter(*database);
      if (!alter.exec(QStringLiteral(
              "ALTER TABLE contacts ADD COLUMN isPending INTEGER NOT NULL "
              "DEFAULT 0"))) {
        if (lastError != nullptr) {
          *lastError = alter.lastError().text();
        }
        return false;
      }
    }
  }

  // Migration for databases created before the `memberEnrolled` column
  // existed. Tracks whether ConnectionManager::enrollMember() for this contact
  // was ever acknowledged by the server; while 0, retryPendingMemberEnrollments
  // resends the request on every reconnect, since a connection drop at the
  // exact moment of the one-shot enroll attempt would otherwise silently and
  // permanently leave the contact provisional (blocked from file transfer)
  // with no user-visible error. Existing rows default to 0 so upgrading
  // clients get one harmless idempotent re-sync sweep.
  {
    bool hasMemberEnrolled = false;
    QSqlQuery pragma(*database);
    if (pragma.exec(QStringLiteral("PRAGMA table_info(contacts)"))) {
      while (pragma.next()) {
        if (pragma.value(1).toString() == QStringLiteral("memberEnrolled")) {
          hasMemberEnrolled = true;
          break;
        }
      }
    }
    if (!hasMemberEnrolled) {
      QSqlQuery alter(*database);
      if (!alter.exec(QStringLiteral(
              "ALTER TABLE contacts ADD COLUMN memberEnrolled INTEGER NOT "
              "NULL DEFAULT 0"))) {
        if (lastError != nullptr) {
          *lastError = alter.lastError().text();
        }
        return false;
      }
    }
  }

  // Self-healing column migration for the messages table.
  //
  // Every message read/write lists its columns explicitly, so a SINGLE missing
  // column makes the whole INSERT fail with "no such column" and the message
  // silently never persists (it still shows live in the in-memory model and
  // still updates the contact's last-message preview, then vanishes on the next
  // load). Probing each added column with its own block risked a half-applied
  // set slipping through — e.g. an interrupted upgrade, or an old database whose
  // messages table predates `timerValue`, for which there was never an ALTER at
  // all. The text INSERT references `timerValue` while the media INSERT does
  // not, so a missing `timerValue` presented as "text messages disappear but
  // files/media stay" — exactly the reported symptom.
  //
  // Instead of trusting any prior migration, read the full column set once and
  // ADD whatever is missing. Only constant-default columns are listed:
  // `receive_time DEFAULT CURRENT_TIMESTAMP` is deliberately omitted because
  // SQLite rejects ALTER TABLE ADD COLUMN with a non-constant default (and no
  // INSERT references it anyway).
  {
    QSet<QString> existing;
    QSqlQuery pragma(*database);
    if (pragma.exec(QStringLiteral("PRAGMA table_info(messages)"))) {
      while (pragma.next()) {
        existing.insert(pragma.value(1).toString());
      }
    }

    const QList<QPair<QString, QString>> requiredColumns = {
        {QStringLiteral("content"), QStringLiteral("BLOB")},
        {QStringLiteral("readTime"), QStringLiteral("INTEGER DEFAULT 0")},
        {QStringLiteral("timerValue"), QStringLiteral("INTEGER DEFAULT 0")},
        {QStringLiteral("callDurationSec"),
         QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("replyTo"),
         QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("replyPreview"),
         QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("replyKind"),
         QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("replyIsOwn"),
         QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
    };

    for (const auto& column : requiredColumns) {
      if (existing.contains(column.first)) {
        continue;
      }
      QSqlQuery alter(*database);
      if (!alter.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN %1 %2")
                          .arg(column.first, column.second))) {
        if (lastError != nullptr) {
          *lastError = alter.lastError().text();
        }
        return false;
      }
    }
  }

  return true;
}