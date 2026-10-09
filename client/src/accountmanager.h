#pragma once

#include <corecrypto.h>

#include <QObject>
#include <QVariantMap>

#include "databasemanager.h"
#include "databasesettingsservice.h"
#include "imageprovider.h"
#include "pqdh.h"
#include "prekeys.h"
#include "settingsmanager.h"

class AccountManager : public QObject {
  Q_OBJECT
 public:
  explicit AccountManager(DatabaseManager* db, QObject* parent = nullptr,
                          SettingsManager* settingsManager = nullptr,
                          AvatarProvider* avatarProvider = nullptr);

  static QString contactAvatarId(const QString& serverId,
                                 const QString& contactPubKey) {
    return serverId + QStringLiteral("||") + contactPubKey;
  }

  Q_INVOKABLE bool create(const QString& mnemonicphrase);
  Q_INVOKABLE QString getPublicKey();
  Q_INVOKABLE QByteArray getPublicKeyRaw();
  Q_INVOKABLE QByteArray getPrivateKey();

  // Shareable contact string (pubkey + invite secret + home server, checksummed
  // and base64url-encoded) — the payload of the "My QR" page.
  Q_INVOKABLE QString contactShareString();
  // Parses a scanned/pasted contact string. Returns {valid, pubKey, server,
  // invite} where pubKey/invite are hex strings.
  Q_INVOKABLE QVariantMap parseContactString(const QString& text) const;
  // Replaces the invite secret: old QR codes/contact strings stop working for
  // NEW sessions; established contacts are unaffected (the invite only feeds
  // the initial ratchet root key).
  Q_INVOKABLE void rotateInviteSecret();
  // Current invite secret, lazily generated on first access. C++ side of the
  // receiver's session gate; QML never needs the raw secret.
  QByteArray getOrCreateInviteSecret();
  Q_INVOKABLE prekeys::PreKey generatePreKey(bool _commit = true);
  Q_INVOKABLE void commit() { m_db->commitTransaction(); }
  Q_INVOKABLE bool hasAvatar(const QString& contactPubKey) {
    if (m_avatarProvider == nullptr) {
      return false;
    }
    return m_avatarProvider->hasAvatar(contactPubKey);
  }
  Q_INVOKABLE bool hasContactAvatar(const QString& serverId,
                                    const QString& contactPubKey) {
    if (m_avatarProvider == nullptr) {
      return false;
    }
    return m_avatarProvider->hasAvatar(
        contactAvatarId(serverId, contactPubKey));
  }
  Q_INVOKABLE QString getContactAvatarSource(
      const QString& serverId, const QString& contactPubKey) const {
    return QStringLiteral("image://avatars/") +
           contactAvatarId(serverId, contactPubKey);
  }
  void setContactAvatar(const QString& serverId, const QString& contactPubKey,
                        const QByteArray& avatarBlob);
  void clearContactAvatar(const QString& serverId,
                          const QString& contactPubKey);
  Q_INVOKABLE void deleteAvatar() {
    if (m_avatarProvider == nullptr) {
      return;
    }
    m_avatarProvider->removeAvatar("0");
  }

 signals:
  void avatarChanged();
  // Emitted when the invite secret is rotated so the QR page can refresh.
  void inviteChanged();

 public slots:
  void selectAndSaveAvatar();
  void loadOwnAvatar();
  void reloadIdentityCache();
  void onAvatarDeleted() {
    qDebug() << "[AccountManager] Avatar deleted signal received";
    deleteAvatar();
  }

 private:
  DatabaseManager* m_db;
  SettingsManager* m_settingsManager;
  AvatarProvider* m_avatarProvider;
  DatabaseSettingsService m_settingsService;

  QByteArray privateKeyRaw_;
  QByteArray publicKeyRaw_;
  QString publicKey_;
  // Invite secret gating new sessions. Persisted synchronously in blobvalues
  // (same reliable path as the identity keys, NOT the async blobsettings cache)
  // so both the QR we hand out and the receiver-side ratchet seed always read
  // the exact same bytes.
  QByteArray inviteSecretRaw_;
};