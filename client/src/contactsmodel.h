// contactsmodel.h
#ifndef CONTACTSMODEL_H
#define CONTACTSMODEL_H

#include <QAbstractListModel>
#include <QObject>
#include <optional>

#include "mainsignals.h"

class DatabaseManager;

struct Contact {
  QString firstName = "";
  QString lastName = "";
  QString lastMessage = "";
  QString time = "";
  QString avatar = "";
  QString avatarSource = "";
  QByteArray avatarBlob;
  int unread = 0;
  bool isOnline = false;
  QString email = "";
  QString phone = "";
  QString server = "";
  int sended = 0;
  bool readed = false;
  QString pubkeyFingerprint = "";
  QString sessionFingerprint = "0000-0000-0000-0000-0000";
  QString lastOnline = "never";
  QString aboutMe = "";
  QString nameStyle = "";
  int timerValue = 0;
  // Set once this contact has published a key-revocation certificate ("HACKED").
  // Terminal: the key must never be trusted again.
  bool revoked = false;
};

Q_DECLARE_METATYPE(Contact)
Q_DECLARE_METATYPE(QList<Contact>)

struct ContactCallInfo {
  QString firstName = "";
  QString avatarSource = "";
  QString server = "";
  QString pubkeyFingerprint = "";
};

class ContactsModel : public QAbstractListModel {
  Q_OBJECT

 public:
  enum ContactRoles {
    FirstNameRole = Qt::UserRole + 1,
    LastNameRole,
    LastMessageRole,
    TimeRole,
    AvatarRole,
    AvatarSourceRole,
    UnreadRole,
    IsOnlineRole,
    EmailRole,
    PhoneRole,
    ServerRole,
    SendedRole,
    ReadedRole,
    PubkeyFingerprintRole,
    SessionFingerprintRole,
    LastOnlineRole,
    AboutMeRole,
    NameStyleRole,
    RevokedRole
  };

  Q_ENUM(ContactRoles)

  explicit ContactsModel(QObject* parent = nullptr,
                         DatabaseManager* dbManager = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index,
                int role = Qt::DisplayRole) const override;
  QHash<int, QByteArray> roleNames() const override;

  Q_INVOKABLE void loadFromDatabase();
  void setContacts(const QList<Contact>& contacts);

  Q_INVOKABLE QVariantMap get(int index) const;
  Q_INVOKABLE std::optional<Contact> getContact(const QString& serverId,
                                                const QString& pubkey) const;
  std::optional<ContactCallInfo> getCallContactInfo(
      const QString& serverId, const QString& pubkey) const;
  QList<QPair<QString, QString>> contactEndpoints() const;
  Q_INVOKABLE bool hasContact(const QString& serverId,
                              const QString& pubkey) const;

  Q_INVOKABLE void addContact(const QVariantMap& contactData);
  void addAnonymousContact(const QString& serverId,
                           const QString& pubkeyFingerprint,
                           const QString& sessionFingerprint,
                           const QString& lastMessage);
  Q_INVOKABLE void removeContactByIndex(int index);
  Q_INVOKABLE void removeContactByPubKey(const QString& serverId,
                                         const QString& pubkey);
  Q_INVOKABLE void updateContact(const QString& serverId,
                                 const QString& pubkeyFingerprint,
                                 const QVariantMap& contactData);
  Q_INVOKABLE void setLastMessage(const QString& serverId,
                                  const QString& pubkeyFingerprint,
                                  const QString& message, const QString& time);
  Q_INVOKABLE void setLastMessageAt(const QString& serverId,
                                    const QString& pubkeyFingerprint,
                                    const QString& message, quint64 timestamp);
  Q_INVOKABLE void setLastMessageNow(const QString& serverId,
                                     const QString& pubkeyFingerprint,
                                     const QString& message);
  Q_INVOKABLE void setUnreadedCount(const QString& serverId,
                                    const QString& pubkeyFingerprint,
                                    int unreadCount);
  Q_INVOKABLE void setContactOnline(const QString& serverId,
                                    const QString& pubkeyFingerprint,
                                    bool isOnline);

  Q_INVOKABLE void incrementUnreadedCount(const QString& serverId,
                                          const QString& contactPubKey);

  Q_INVOKABLE QString getFirstName(int index) const;
  Q_INVOKABLE QString getLastName(int index) const;
  Q_INVOKABLE QString getAvatar(int index) const;
  // Q_INVOKABLE QByteArray getAvatarBlob(int index) const;
  Q_INVOKABLE QString getAvatarSource(int index) const;
  Q_INVOKABLE QString getAboutMe(int index) const;
  Q_INVOKABLE QString getNameStyle(int index) const;
  Q_INVOKABLE bool getIsOnline(int index) const;
  Q_INVOKABLE bool getRevoked(int index) const;
  Q_INVOKABLE QString getPubKeyFingerprint(int index) const;
  Q_INVOKABLE QString getSessionFingerprint(int index) const;
  Q_INVOKABLE QString getServer(int index) const;
  Q_INVOKABLE int getTimerValue(int index) const;
  Q_INVOKABLE QString getTimerValueS(int timerValue) const;
  Q_INVOKABLE QVariantMap getContacts() const;
  Q_INVOKABLE QString getLastOnlineDescription(int index) const;
  Q_INVOKABLE QString getLastNameByKey(const QString& serverId,
                                       const QString& pubKey) const;
  Q_INVOKABLE QString getNameStyleByKey(const QString& serverId,
                                        const QString& pubKey) const;

 public slots:
  void onContactRemove(const QString& serverId,
                       const QString& pubkeyFingerprint);
  void onSetTimer(const QString& serverId, const QString& contactPubKey,
                  int seconds);
  void onContactsLoaded(const QList<Contact>& contacts);

 signals:
  void contactUpdated(int index);
  void contactRemoved(int index);
  void lastOnlineUpdated(int index);
  // Emitted after a contact row is relocated (currently only "bump to top").
  // QML uses this to keep the open-chat currentIndex pinned to its contact,
  // since QQuickListView does not adjust currentIndex on row moves.
  void contactMoved(int from, int to);

 private:
  int findContactIndex(const QString& serverId,
                       const QString& pubkeyFingerprint) const;
  // Moves the contact at the given index to the top of the list (newest-first
  // ordering). No-op when index <= 0 (not found or already on top).
  void moveContactToTop(int index);
  QList<Contact> m_contacts;
  DatabaseManager* m_db;
};

#endif  // CONTACTSMODEL_H