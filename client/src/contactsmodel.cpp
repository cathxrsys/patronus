// contactsmodel.cpp
#include "contactsmodel.h"

#include "databasemanager.h"

ContactsModel::ContactsModel(QObject* parent, DatabaseManager* dbManager)
    : QAbstractListModel(parent), m_db(dbManager) {
  if (m_db != nullptr) {
    connect(m_db, &DatabaseManager::contactsLoaded, this,
            &ContactsModel::onContactsLoaded);
  }

  connect(&MainSignals::instance(), &MainSignals::contactRemove, this,
          &ContactsModel::onContactRemove);
  connect(&MainSignals::instance(), &MainSignals::sessionRemoveReceived, this,
          &ContactsModel::onContactRemove);
  connect(&MainSignals::instance(), &MainSignals::setTimer, this,
          &ContactsModel::onSetTimer);
  connect(&MainSignals::instance(), &MainSignals::setTimerReceived, this,
          &ContactsModel::onSetTimer);
  connect(&MainSignals::instance(), &MainSignals::sessionFingerprintChanged,
          this,
          [this](const QString& serverId, const QString& contactPubKey,
                 const QString& sessionFingerprint) {
            const int contactIndex = findContactIndex(serverId, contactPubKey);
            if (contactIndex < 0) {
              return;
            }

            Contact& contact = m_contacts[contactIndex];
            if (contact.sessionFingerprint == sessionFingerprint) {
              return;
            }

            contact.sessionFingerprint = sessionFingerprint;

            const QModelIndex idx = index(contactIndex);
            emit dataChanged(idx, idx, {SessionFingerprintRole});
            emit contactUpdated(contactIndex);
          });

  connect(&MainSignals::instance(),
          &MainSignals::identityRevocationReceived, this,
          [this](const QString& serverId, const QString& contactPubKey) {
            const int contactIndex = findContactIndex(serverId, contactPubKey);
            if (contactIndex < 0) {
              return;
            }

            Contact& contact = m_contacts[contactIndex];
            if (contact.revoked) {
              return;
            }

            contact.revoked = true;

            const QModelIndex idx = index(contactIndex);
            emit dataChanged(idx, idx, {RevokedRole});
            emit contactUpdated(contactIndex);
          });
}

int ContactsModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return m_contacts.count();
}

QVariant ContactsModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= m_contacts.count()) {
    return QString("");
  }

  const Contact& contact = m_contacts[index.row()];

  switch (role) {
    case FirstNameRole:
      return contact.firstName;
    case LastNameRole:
      return contact.lastName;
    case LastMessageRole:
      return contact.lastMessage.length() > 0
                 ? contact.lastMessage
                 : tr("Contact added. Write your first message!");
    case TimeRole:
      return contact.time;
    case AvatarRole:
      return contact.avatar;
    case AvatarSourceRole:
      return contact.avatarSource;
    case UnreadRole:
      return contact.unread;
    case IsOnlineRole:
      return contact.isOnline;
    case EmailRole:
      return contact.email;
    case PhoneRole:
      return contact.phone;
    case ServerRole:
      return contact.server;
    case SendedRole:
      return contact.sended;
    case ReadedRole:
      return contact.readed;
    case PubkeyFingerprintRole:
      return contact.pubkeyFingerprint;
    case SessionFingerprintRole:
      return contact.sessionFingerprint;
    case LastOnlineRole:
      return contact.lastOnline;
    case AboutMeRole:
      return contact.aboutMe;
    case NameStyleRole:
      return contact.nameStyle;
    case RevokedRole:
      return contact.revoked;
    default:
      return QString("");
  }
}

QHash<int, QByteArray> ContactsModel::roleNames() const {
  QHash<int, QByteArray> roles;
  roles[FirstNameRole] = "firstName";
  roles[LastNameRole] = "lastName";
  roles[LastMessageRole] = "lastMessage";
  roles[TimeRole] = "time";
  roles[AvatarRole] = "avatar";
  roles[AvatarSourceRole] = "avatarSource";
  roles[UnreadRole] = "unread";
  roles[IsOnlineRole] = "isOnline";
  roles[EmailRole] = "email";
  roles[PhoneRole] = "phone";
  roles[ServerRole] = "server";
  roles[SendedRole] = "sended";
  roles[ReadedRole] = "readed";
  roles[PubkeyFingerprintRole] = "pubkeyFingerprint";
  roles[SessionFingerprintRole] = "sessionFingerprint";
  roles[LastOnlineRole] = "lastOnline";
  roles[AboutMeRole] = "aboutMe";
  roles[NameStyleRole] = "nameStyle";
  roles[RevokedRole] = "revoked";
  return roles;
}

void ContactsModel::loadFromDatabase() {
  if (m_db == nullptr) {
    setContacts({});
    return;
  }

  m_db->requestContactsLoad();
}
int ContactsModel::findContactIndex(const QString& serverId,
                                    const QString& pubkeyFingerprint) const {
  for (int i = 0; i < m_contacts.count(); ++i) {
    if (m_contacts[i].server == serverId &&
        m_contacts[i].pubkeyFingerprint == pubkeyFingerprint) {
      return i;
    }
  }

  return -1;
}

void ContactsModel::onContactsLoaded(const QList<Contact>& contacts) {
  setContacts(contacts);
}

void ContactsModel::setContacts(const QList<Contact>& contacts) {
  beginResetModel();
  m_contacts = contacts;
  endResetModel();
}

QVariantMap ContactsModel::get(int index) const {
  QVariantMap map;

  map["firstName"] = "";
  map["lastName"] = "";
  map["lastMessage"] = "";
  map["time"] = "";
  map["avatar"] = "";
  map["avatarSource"] = "";
  map["unread"] = 0;
  map["isOnline"] = false;
  map["email"] = "";
  map["phone"] = "";
  map["server"] = "";
  map["sended"] = 0;
  map["readed"] = false;
  map["pubkeyFingerprint"] = "";
  map["sessionFingerprint"] = "";
  map["lastOnline"] = "";
  map["aboutMe"] = "";
  map["nameStyle"] = "";
  map["timerValue"] = 0;
  map["revoked"] = false;
  map["avatarBlob"] = QByteArray();

  if (index >= 0 && index < m_contacts.count()) {
    const Contact& contact = m_contacts[index];

    map["firstName"] = contact.firstName;
    map["lastName"] = contact.lastName;
    map["lastMessage"] = contact.lastMessage;
    map["time"] = contact.time;
    map["avatar"] = contact.avatar;
    map["avatarSource"] = contact.avatarSource;
    map["unread"] = contact.unread;
    map["isOnline"] = contact.isOnline;
    map["email"] = contact.email;
    map["phone"] = contact.phone;
    map["server"] = contact.server;
    map["sended"] = contact.sended;
    map["readed"] = contact.readed;
    map["pubkeyFingerprint"] = contact.pubkeyFingerprint;
    map["sessionFingerprint"] = contact.sessionFingerprint;
    map["lastOnline"] = contact.lastOnline;
    map["aboutMe"] = contact.aboutMe;
    map["nameStyle"] = contact.nameStyle;
    map["timerValue"] = contact.timerValue;
    map["revoked"] = contact.revoked;
    if (contact.avatarBlob.size() > 0) {
      map["avatarBlob"] = contact.avatarBlob;
    }
  }

  return map;
}

QVariantMap ContactsModel::getContacts() const {
  QVariantMap contactsMap;
  for (int i = 0; i < m_contacts.count(); ++i) {
    contactsMap[QString::number(i)] = get(i);
  }
  return contactsMap;
}

void ContactsModel::addContact(const QVariantMap& contactData) {
  if (contactData.isEmpty()) return;
  Contact contact;

  if (!contactData.contains("firstName")) {
    contact.firstName = "Anonymous";
    contact.lastName = "";
    contact.avatarSource = "resources/avatars/anonymous.svg";
  } else {
    contact.firstName = contactData["firstName"].toString();
    contact.lastName = contactData["lastName"].toString();
    contact.avatarSource = "";
    contact.avatar = contact.firstName[0];
  }

  if (!contactData.contains("lastMessage")) {
    contact.lastMessage = tr("Contact added. Write your first message!");
  } else {
    contact.lastMessage = contactData["lastMessage"].toString();
  }

  contact.time = QTime::currentTime().toString("HH:mm");

  contact.server = contactData["server"].toString();
  contact.pubkeyFingerprint = contactData["pubkeyFingerprint"].toString();

  if (contactData.contains("sessionFingerprint")) {
    contact.sessionFingerprint =
        contactData.value("sessionFingerprint").toString();
  }

  if (contactData.contains("avatarBlob")) {
    contact.avatarBlob = contactData.value("avatarBlob").toByteArray();
  }

  if (contactData.contains("nameStyle")) {
    contact.nameStyle = contactData.value("nameStyle").toString();
  }

  beginInsertRows(QModelIndex(), m_contacts.count(), m_contacts.count());
  m_contacts.append(contact);
  endInsertRows();
}

void ContactsModel::addAnonymousContact(const QString& serverId,
                                        const QString& pubkeyFingerprint,
                                        const QString& sessionFingerprint,
                                        const QString& lastMessage) {
  QVariantMap contact;
  contact["firstName"] = "Anonymous";
  contact["lastName"] = "";
  contact["server"] = serverId;
  contact["pubkeyFingerprint"] = pubkeyFingerprint;
  contact["sessionFingerprint"] = sessionFingerprint;
  if (!lastMessage.isEmpty()) {
    contact["lastMessage"] = lastMessage;
  }
  addContact(contact);
}

void ContactsModel::removeContactByIndex(int index) {
  if (index < 0 || index >= m_contacts.count()) return;

  beginRemoveRows(QModelIndex(), index, index);
  const Contact contact = m_contacts[index];
  m_contacts.removeAt(index);
  endRemoveRows();

  // emit contactRemoved(index);
  MainSignals::instance().contactRemovedIndex(index);
  MainSignals::instance().emitContactRemovedLocally(contact.server,
                                                    contact.pubkeyFingerprint);

  if (m_db) {
    m_db->requestContactRemoval(contact.server, contact.pubkeyFingerprint);
  }
}

void ContactsModel::removeContactByPubKey(const QString& serverId,
                                          const QString& pubkey) {
  const int contactIndex = findContactIndex(serverId, pubkey);
  if (contactIndex < 0) {
    return;
  }

  beginRemoveRows(QModelIndex(), contactIndex, contactIndex);
  m_contacts.removeAt(contactIndex);
  endRemoveRows();

  MainSignals::instance().contactRemovedIndex(contactIndex);
  MainSignals::instance().emitContactRemovedLocally(serverId, pubkey);

  if (m_db) {
    m_db->requestContactRemoval(serverId, pubkey);
  }
}

void ContactsModel::updateContact(const QString& serverId,
                                  const QString& pubkeyFingerprint,
                                  const QVariantMap& contactData) {
  const int contactIndex = findContactIndex(serverId, pubkeyFingerprint);
  if (contactIndex < 0) {
    return;
  }

  Contact& contact = m_contacts[contactIndex];

  if (contactData.contains("firstName")) {
    contact.firstName = contactData["firstName"].toString();
  }
  if (contactData.contains("lastName")) {
    contact.lastName = contactData["lastName"].toString();
  }
  if (contactData.contains("lastMessage")) {
    contact.lastMessage = contactData["lastMessage"].toString();
  }
  if (contactData.contains("time")) {
    contact.time = contactData["time"].toString();
  }
  if (contactData.contains("avatar")) {
    contact.avatar = contactData["avatar"].toString();
  }
  if (contactData.contains("avatarSource")) {
    contact.avatarSource = contactData["avatarSource"].toString();
  }
  if (contactData.contains("unread")) {
    contact.unread = contactData["unread"].toInt();
  }
  if (contactData.contains("isOnline")) {
    contact.isOnline = contactData["isOnline"].toBool();
  }
  if (contactData.contains("email")) {
    contact.email = contactData["email"].toString();
  }
  if (contactData.contains("phone")) {
    contact.phone = contactData["phone"].toString();
  }
  if (contactData.contains("server")) {
    contact.server = contactData["server"].toString();
  }
  if (contactData.contains("sended")) {
    contact.sended = contactData["sended"].toInt();
  }
  if (contactData.contains("readed")) {
    contact.readed = contactData["readed"].toBool();
  }
  if (contactData.contains("sessionFingerprint")) {
    contact.sessionFingerprint = contactData["sessionFingerprint"].toString();
  }
  if (contactData.contains("lastOnline")) {
    contact.lastOnline = contactData["lastOnline"].toString();
  }
  if (contactData.contains("aboutMe")) {
    contact.aboutMe = contactData["aboutMe"].toString();
  }
  if (contactData.contains("nameStyle")) {
    contact.nameStyle = contactData["nameStyle"].toString();
  }
  if (contactData.contains("avatarBlob")) {
    contact.avatarBlob = contactData["avatarBlob"].toByteArray();

    qDebug() << "[ContactsModel] contactData['avatarBlob'] size:"
             << contact.avatarBlob.size();
    qDebug() << "[ContactsModel] Updating avatarBlob for contact with "
                "pubkeyFingerprint:"
             << contact.pubkeyFingerprint;
  }

  QModelIndex idx = index(contactIndex);

  qDebug() << "[ContactsModel] Emitting dataChanged for index" << contactIndex
           << "pubkeyFingerprint:" << contact.pubkeyFingerprint;

  emit dataChanged(idx, idx);
  emit contactUpdated(contactIndex);

  if (m_db) {
    m_db->requestContactUpdate(contact.server, contact.pubkeyFingerprint,
                               contact.firstName, contact.lastName,
                               contact.aboutMe, contact.nameStyle,
                               contact.avatarBlob);
  }
}

Q_INVOKABLE QString ContactsModel::getFirstName(int index) const {
  if (index < 0 || index >= m_contacts.count()) return "";
  return m_contacts[index].firstName;
}

Q_INVOKABLE QString ContactsModel::getLastName(int index) const {
  if (index < 0 || index >= m_contacts.count()) return "";
  return m_contacts[index].lastName.length() > 0 ? m_contacts[index].lastName
                                                 : "";
}

Q_INVOKABLE QString ContactsModel::getAvatar(int index) const {
  if (index < 0 || index >= m_contacts.count()) return "";
  return m_contacts[index].avatar;
}

Q_INVOKABLE QString ContactsModel::getAvatarSource(int index) const {
  if (index < 0 || index >= m_contacts.count()) return "";
  return m_contacts[index].avatarSource;
}

Q_INVOKABLE QString ContactsModel::getAboutMe(int index) const {
  if (index < 0 || index >= m_contacts.count()) return "";
  return m_contacts[index].aboutMe;
}

Q_INVOKABLE QString ContactsModel::getNameStyle(int index) const {
  if (index < 0 || index >= m_contacts.count()) return "";
  return m_contacts[index].nameStyle;
}

Q_INVOKABLE QString ContactsModel::getLastNameByKey(const QString& serverId,
                                                     const QString& pubKey) const {
  const int idx = findContactIndex(serverId, pubKey);
  if (idx < 0) return "";
  return m_contacts[idx].lastName;
}

Q_INVOKABLE QString ContactsModel::getNameStyleByKey(const QString& serverId,
                                                      const QString& pubKey) const {
  const int idx = findContactIndex(serverId, pubKey);
  if (idx < 0) return "";
  return m_contacts[idx].nameStyle;
}

Q_INVOKABLE bool ContactsModel::getIsOnline(int index) const {
  if (index < 0 || index >= m_contacts.count()) return false;
  return m_contacts[index].isOnline;
}

Q_INVOKABLE bool ContactsModel::getRevoked(int index) const {
  if (index < 0 || index >= m_contacts.count()) return false;
  return m_contacts[index].revoked;
}

Q_INVOKABLE QString ContactsModel::getPubKeyFingerprint(int index) const {
  if (index < 0 || index >= m_contacts.count()) return "";
  return m_contacts[index].pubkeyFingerprint;
}

Q_INVOKABLE QString ContactsModel::getSessionFingerprint(int index) const {
  if (index < 0 || index >= m_contacts.count())
    return "0000-0000-0000-0000-0000";
  return m_contacts[index].sessionFingerprint;
}

Q_INVOKABLE QString ContactsModel::getServer(int index) const {
  if (index < 0 || index >= m_contacts.count()) return "";
  return m_contacts[index].server;
}

Q_INVOKABLE int ContactsModel::getTimerValue(int index) const {
  if (index < 0 || index >= m_contacts.count()) return 0;
  return m_contacts[index].timerValue;
}

Q_INVOKABLE QString ContactsModel::getTimerValueS(int timerValue) const {
  if (m_db == nullptr) {
    return QString();
  }

  return m_db->getTimerValueS(timerValue);
}

Q_INVOKABLE void ContactsModel::setContactOnline(
    const QString& serverId, const QString& pubkeyFingerprint, bool isOnline) {
  const int contactIndex = findContactIndex(serverId, pubkeyFingerprint);
  if (contactIndex < 0) {
    return;
  }

  Contact& contact = m_contacts[contactIndex];
  contact.isOnline = isOnline;

  if (isOnline) {
    contact.lastOnline = QDateTime::currentDateTime().toString(Qt::ISODate);
    emit MainSignals::instance().emitLastOnlineChanged(
        contact.server, contact.pubkeyFingerprint, contact.lastOnline);
  }

  const QModelIndex idx = index(contactIndex);

  emit dataChanged(idx, idx);
  emit contactUpdated(contactIndex);
  emit lastOnlineUpdated(contactIndex);
}

Q_INVOKABLE void ContactsModel::setLastMessage(const QString& serverId,
                                               const QString& pubkeyFingerprint,
                                               const QString& message,
                                               const QString& time) {
  const int contactIndex = findContactIndex(serverId, pubkeyFingerprint);
  if (contactIndex < 0) {
    return;
  }

  Contact& contact = m_contacts[contactIndex];
  contact.lastMessage = message;

  if (contact.lastMessage.count("\n") == 0) {
    contact.lastMessage += "\n";
  }

  contact.time = time;

  const QModelIndex idx = index(contactIndex);

  emit dataChanged(idx, idx);
  emit contactUpdated(contactIndex);
}

void ContactsModel::setLastMessageAt(const QString& serverId,
                                     const QString& pubkeyFingerprint,
                                     const QString& message,
                                     quint64 timestamp) {
  setLastMessage(serverId, pubkeyFingerprint, message,
                 QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp))
                     .toString(QStringLiteral("HH:mm")));
  // A fresh message (incoming or outgoing) bumps the contact to the top so the
  // list stays ordered newest-first. Delete/clear paths use the plain
  // setLastMessage() overload above and intentionally keep their position.
  moveContactToTop(findContactIndex(serverId, pubkeyFingerprint));
}

void ContactsModel::setLastMessageNow(const QString& serverId,
                                      const QString& pubkeyFingerprint,
                                      const QString& message) {
  setLastMessage(
      serverId, pubkeyFingerprint, message,
      QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
  moveContactToTop(findContactIndex(serverId, pubkeyFingerprint));
}

void ContactsModel::moveContactToTop(int index) {
  // index < 0 -> contact not found; index == 0 -> already on top.
  if (index <= 0 || index >= m_contacts.count()) {
    return;
  }

  // beginMoveRows validates the move; destination row 0 is outside the moved
  // range so this always succeeds, but guard defensively regardless.
  if (!beginMoveRows(QModelIndex(), index, index, QModelIndex(), 0)) {
    return;
  }
  m_contacts.move(index, 0);
  endMoveRows();

  emit contactMoved(index, 0);
}

Q_INVOKABLE void ContactsModel::setUnreadedCount(
    const QString& serverId, const QString& pubkeyFingerprint,
    int unreadCount) {
  const int contactIndex = findContactIndex(serverId, pubkeyFingerprint);
  if (contactIndex < 0) {
    return;
  }

  Contact& contact = m_contacts[contactIndex];
  contact.unread = unreadCount;

  const QModelIndex idx = index(contactIndex);

  emit dataChanged(idx, idx);
  emit contactUpdated(contactIndex);
}

void ContactsModel::incrementUnreadedCount(const QString& serverId,
                                           const QString& contactPubKey) {
  const int contactIndex = findContactIndex(serverId, contactPubKey);
  if (contactIndex < 0) {
    return;
  }

  Contact& contact = m_contacts[contactIndex];
  contact.unread += 1;

  const QModelIndex idx = index(contactIndex);

  emit dataChanged(idx, idx);
  emit contactUpdated(contactIndex);
}

Q_INVOKABLE std::optional<Contact> ContactsModel::getContact(
    const QString& serverId, const QString& pubkey) const {
  const int contactIndex = findContactIndex(serverId, pubkey);
  if (contactIndex >= 0) {
    return m_contacts[contactIndex];
  }
  return std::nullopt;
}

std::optional<ContactCallInfo> ContactsModel::getCallContactInfo(
    const QString& serverId, const QString& pubkey) const {
  const int contactIndex = findContactIndex(serverId, pubkey);
  if (contactIndex >= 0) {
    const Contact& contact = m_contacts[contactIndex];
    ContactCallInfo info;
    info.firstName = contact.firstName;
    info.avatarSource = contact.avatarSource;
    info.server = contact.server;
    info.pubkeyFingerprint = contact.pubkeyFingerprint;
    return info;
  }

  return std::nullopt;
}

QList<QPair<QString, QString>> ContactsModel::contactEndpoints() const {
  QList<QPair<QString, QString>> endpoints;
  endpoints.reserve(m_contacts.size());

  for (const Contact& contact : m_contacts) {
    endpoints.append(qMakePair(contact.server, contact.pubkeyFingerprint));
  }

  return endpoints;
}

Q_INVOKABLE bool ContactsModel::hasContact(const QString& serverId,
                                           const QString& pubkey) const {
  return getContact(serverId, pubkey).has_value();
}

Q_INVOKABLE QString ContactsModel::getLastOnlineDescription(int index) const {
  if (index < 0 || index >= m_contacts.count()) return tr("never");

  const Contact& contact = m_contacts[index];

  if (contact.isOnline) {
    return tr("online");
  }

  if (contact.lastOnline == "") {
    return tr("never");
  }

  QDateTime lastOnlineDT =
      QDateTime::fromString(contact.lastOnline, Qt::ISODate);
  if (!lastOnlineDT.isValid()) {
    return tr("never");
  }

  qint64 secondsDiff = lastOnlineDT.secsTo(QDateTime::currentDateTime());

  if (secondsDiff < 60) {
    return tr("just now");
  } else if (secondsDiff < 3600) {
    int minutes = secondsDiff / 60;
    return tr("was %1 minute(s) ago").arg(minutes);
  } else if (secondsDiff < 86400) {
    int hours = secondsDiff / 3600;
    return tr("was %1 hour(s) ago").arg(hours);
  } else {
    int days = secondsDiff / 86400;
    return tr("was %1 day(s) ago").arg(days);
  }
}

void ContactsModel::onContactRemove(const QString& serverId,
                                    const QString& contactPubKey) {
  qDebug() << "[ContactsModel] onContactRemove called with contactPubKey:"
           << contactPubKey << "on server:" << serverId;
  removeContactByPubKey(serverId, contactPubKey);
}

void ContactsModel::onSetTimer(const QString& serverId,
                               const QString& contactPubKey, int seconds) {
  qDebug() << "[ContactsModel] onSetTimer called with contactPubKey:"
           << contactPubKey << "on server:" << serverId
           << "seconds:" << seconds;

  const int contactIndex = findContactIndex(serverId, contactPubKey);
  if (contactIndex < 0) {
    return;
  }

  Contact& contact = m_contacts[contactIndex];
  contact.timerValue = seconds;

  if (seconds == 0) {
    contact.lastMessage = tr("Timer is disabled") + "\n";
  } else {
    contact.lastMessage =
        tr("Timer is set for %1").arg(m_db->getTimerValueS(seconds)) + "\n";
  }

  contact.time = QTime::currentTime().toString("HH:mm");

  const QModelIndex idx = index(contactIndex);

  emit dataChanged(idx, idx);
}