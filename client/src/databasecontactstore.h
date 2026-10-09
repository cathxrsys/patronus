#pragma once

#include <QSqlDatabase>
#include <QStringList>

#include "contactsmodel.h"
#include "doubleratchet.h"

class DatabaseContactStore {
 public:
  explicit DatabaseContactStore(QSqlDatabase* database = nullptr);

  void setDatabase(QSqlDatabase* database);

  QList<Contact> loadContactsBase() const;
  void updateContactRecord(const QString& serverId,
                           const QString& contactPubKey,
                           const QString& firstName, const QString& lastName,
                           const QString& aboutMe, const QString& nameStyle,
                           const QByteArray& avatarBlob) const;
  bool contactExists(const QString& serverId,
                     const QString& pubKeyFingerprint) const;
  dr::DoubleRatchet loadDoubleRatchet(const QString& serverId,
                                      const QString& pubKeyFingerprint) const;
  void saveDoubleRatchet(const QString& serverId,
                         const QString& pubKeyFingerprint,
                         const dr::DoubleRatchet& doubleRatchet) const;
  QStringList loadKnownServerAddresses() const;
  bool storeAnonymousContactSession(const QString& serverId,
                                    const QString& contactPubKey,
                                    const QString& doubleRatchetJson) const;
  int contactTimerValue(const QString& serverId,
                        const QString& contactPubKey) const;
  void deleteContactByPubKey(const QString& serverId,
                             const QString& contactPubKey) const;
  void updateLastOnline(const QString& serverId, const QString& contactPubKey,
                        const QString& lastOnline) const;
  void updateContactTimerValue(const QString& serverId,
                               const QString& contactPubKey,
                               int timerValue) const;
  void markContactRevoked(const QString& serverId,
                          const QString& contactPubKey) const;

 private:
  bool ensureOpen() const;

  QSqlDatabase* m_database = nullptr;
};