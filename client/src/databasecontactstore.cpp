#include "databasecontactstore.h"

#include <QDebug>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

#include "nlohmann/json.hpp"

DatabaseContactStore::DatabaseContactStore(QSqlDatabase* database)
    : m_database(database) {}

void DatabaseContactStore::setDatabase(QSqlDatabase* database) {
  m_database = database;
}

bool DatabaseContactStore::ensureOpen() const {
  if (m_database == nullptr) {
    qCritical() << "ContactStore: database is null";
    return false;
  }

  if (m_database->isOpen()) {
    return true;
  }

  if (!m_database->open()) {
    qCritical() << "ContactStore: DB not open and cannot reopen!";
    return false;
  }

  return true;
}

QList<Contact> DatabaseContactStore::loadContactsBase() const {
  QList<Contact> contacts;
  if (!ensureOpen()) {
    return contacts;
  }

  QSqlQuery query(*m_database);
  // Pending rows are invite-gated sessions that have not yet proven knowledge
  // of our invite secret — they must never surface as contacts.
  query.prepare("SELECT * FROM contacts WHERE isPending = 0");
  if (!query.exec()) {
    qCritical() << "ContactStore loadContactsBase failed:"
                << query.lastError().text();
    return contacts;
  }

  while (query.next()) {
    Contact contact;
    contact.firstName = query.value("firstName").toString();
    if (contact.firstName.isEmpty()) {
      contact.firstName = QStringLiteral("Anonymous");
      contact.lastName.clear();
      contact.avatarSource = QStringLiteral("resources/avatars/anonymous.svg");
    } else {
      contact.lastName = query.value("lastName").toString();
    }

    contact.server = query.value("serverAddress").toString();
    contact.pubkeyFingerprint = query.value("pubKey").toString();
    contact.aboutMe = query.value("aboutMe").toString();
    contact.nameStyle = query.value("nameStyle").toString();
    contact.timerValue = query.value("timerValue").toInt();
    contact.avatar =
        contact.firstName.isEmpty() ? QString() : contact.firstName[0];
    contact.lastOnline = query.value("lastOnline").toString();
    contact.avatarBlob = query.value("avatar").toByteArray();
    contact.revoked = query.value("revoked").toBool();

    const std::string doubleRatchetJsonString =
        query.value("doubleratchet").toString().toStdString();
    const nlohmann::json doubleRatchetJson =
        nlohmann::json::parse(doubleRatchetJsonString, nullptr, false);
    if (!doubleRatchetJson.is_discarded()) {
      dr::DoubleRatchet doubleRatchet;
      doubleRatchet.from_json(doubleRatchetJson);
      contact.sessionFingerprint =
          QString::fromStdString(doubleRatchet.get_session_key_fingerprint());
    }

    contacts.append(contact);
  }

  return contacts;
}

void DatabaseContactStore::updateContactRecord(
    const QString& serverId, const QString& contactPubKey,
    const QString& firstName, const QString& lastName, const QString& aboutMe,
    const QString& nameStyle, const QByteArray& avatarBlob) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "UPDATE contacts SET firstName = ?, lastName = ?, aboutMe = ?, nameStyle "
      "= ?, avatar = ? WHERE serverAddress = ? AND pubKey = ?");
  query.bindValue(0, firstName);
  query.bindValue(1, lastName);
  query.bindValue(2, aboutMe);
  query.bindValue(3, nameStyle);
  query.bindValue(4, avatarBlob);
  query.bindValue(5, serverId);
  query.bindValue(6, contactPubKey);

  if (!query.exec()) {
    qCritical() << "ContactStore updateContactRecord failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}

bool DatabaseContactStore::contactExists(
    const QString& serverId, const QString& pubKeyFingerprint) const {
  if (!ensureOpen()) {
    return false;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "SELECT COUNT(*) FROM contacts WHERE serverAddress = ? AND pubKey = ?");
  query.bindValue(0, serverId);
  query.bindValue(1, pubKeyFingerprint);
  if (!query.exec() || !query.next()) {
    qCritical() << "ContactStore contactExists failed:"
                << query.lastError().text();
    return false;
  }

  return query.value(0).toInt() > 0;
}

dr::DoubleRatchet DatabaseContactStore::loadDoubleRatchet(
    const QString& serverId, const QString& pubKeyFingerprint) const {
  dr::DoubleRatchet doubleRatchet;
  if (!ensureOpen()) {
    return doubleRatchet;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "SELECT doubleratchet FROM contacts WHERE serverAddress = ? AND pubKey = "
      "?");
  query.bindValue(0, serverId);
  query.bindValue(1, pubKeyFingerprint);
  if (!query.exec()) {
    qCritical() << "ContactStore loadDoubleRatchet failed:"
                << query.lastError().text();
    return doubleRatchet;
  }

  if (query.next()) {
    const std::string doubleRatchetJsonString =
        query.value("doubleratchet").toString().toStdString();
    const nlohmann::json doubleRatchetJson =
        nlohmann::json::parse(doubleRatchetJsonString, nullptr, false);
    if (!doubleRatchetJson.is_discarded()) {
      doubleRatchet.from_json(doubleRatchetJson);
    }
  }

  return doubleRatchet;
}

void DatabaseContactStore::saveDoubleRatchet(
    const QString& serverId, const QString& pubKeyFingerprint,
    const dr::DoubleRatchet& doubleRatchet) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "UPDATE contacts SET doubleratchet = ? WHERE serverAddress = ? AND "
      "pubKey = ?");
  query.bindValue(0, QString::fromStdString(doubleRatchet.to_json().dump()));
  query.bindValue(1, serverId);
  query.bindValue(2, pubKeyFingerprint);
  if (!query.exec()) {
    qCritical() << "ContactStore saveDoubleRatchet failed:"
                << query.lastError().text();
  }
}

QStringList DatabaseContactStore::loadKnownServerAddresses() const {
  QStringList addresses;
  if (!ensureOpen()) {
    return addresses;
  }

  QSet<QString> uniqueAddresses;

  QSqlQuery contactQuery(*m_database);
  if (contactQuery.exec("SELECT serverAddress FROM contacts")) {
    while (contactQuery.next()) {
      const QString address = contactQuery.value(0).toString();
      if (!address.isEmpty()) {
        uniqueAddresses.insert(address);
      }
    }
  }

  QSqlQuery settingQuery(*m_database);
  settingQuery.prepare("SELECT value FROM textsettings WHERE name = ?");
  settingQuery.bindValue(0, QStringLiteral("serverAddress"));
  if (settingQuery.exec()) {
    while (settingQuery.next()) {
      const QString address = settingQuery.value(0).toString();
      if (!address.isEmpty()) {
        uniqueAddresses.insert(address);
      }
    }
  }

  return uniqueAddresses.values();
}

bool DatabaseContactStore::storeAnonymousContactSession(
    const QString& serverId, const QString& contactPubKey,
    const QString& doubleRatchetJson) const {
  if (!ensureOpen()) {
    return false;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "INSERT INTO contacts (pubKey, firstName, lastName, aboutMe, avatar, "
      "serverAddress, doubleratchet) VALUES (?, '', '', '', '', ?, ?)");
  query.bindValue(0, contactPubKey);
  query.bindValue(1, serverId);
  query.bindValue(2, doubleRatchetJson);
  return query.exec();
}

int DatabaseContactStore::contactTimerValue(
    const QString& serverId, const QString& contactPubKey) const {
  if (!ensureOpen()) {
    return -1;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "SELECT timerValue FROM contacts WHERE serverAddress = :serverAddress "
      "AND pubKey = :pubKey");
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":pubKey", contactPubKey);
  if (!query.exec()) {
    qCritical() << "ContactStore contactTimerValue failed:"
                << query.lastError().text();
    return -1;
  }

  return query.next() ? query.value(0).toInt() : -1;
}

void DatabaseContactStore::deleteContactByPubKey(
    const QString& serverId, const QString& contactPubKey) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "DELETE FROM contacts WHERE serverAddress = :serverAddress AND pubKey = "
      ":pubKey");
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":pubKey", contactPubKey);
  if (!query.exec()) {
    qCritical() << "ContactStore deleteContactByPubKey failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}

void DatabaseContactStore::updateLastOnline(const QString& serverId,
                                            const QString& contactPubKey,
                                            const QString& lastOnline) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "UPDATE contacts SET lastOnline = :lastOnline WHERE serverAddress = "
      ":serverAddress AND pubKey = :pubKey");
  query.bindValue(":lastOnline", lastOnline);
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":pubKey", contactPubKey);
  if (!query.exec()) {
    qCritical() << "ContactStore updateLastOnline failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}

void DatabaseContactStore::updateContactTimerValue(const QString& serverId,
                                                   const QString& contactPubKey,
                                                   int timerValue) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "UPDATE contacts SET timerValue = :timerValue WHERE serverAddress = "
      ":serverAddress AND pubKey = :pubKey");
  query.bindValue(":timerValue", timerValue);
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":pubKey", contactPubKey);
  if (!query.exec()) {
    qCritical() << "ContactStore updateContactTimerValue failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}

void DatabaseContactStore::markContactRevoked(
    const QString& serverId, const QString& contactPubKey) const {
  if (!ensureOpen()) {
    return;
  }

  QSqlQuery query(*m_database);
  query.prepare(
      "UPDATE contacts SET revoked = 1 WHERE serverAddress = :serverAddress "
      "AND pubKey = :pubKey");
  query.bindValue(":serverAddress", serverId);
  query.bindValue(":pubKey", contactPubKey);
  if (!query.exec()) {
    qCritical() << "ContactStore markContactRevoked failed:"
                << query.lastError().text() << "\nQuery:" << query.lastQuery();
  }
}