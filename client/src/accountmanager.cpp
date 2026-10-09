#include "accountmanager.h"

#include <corecrypto.h>

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QStandardPaths>

#include "databasemanager.h"
#include "filepicker.h"
#include "inviteutils.h"
#include "mainsignals.h"
#include "settingsmanager.h"

AccountManager::AccountManager(DatabaseManager* db, QObject* parent,
                               SettingsManager* settingsManager,
                               AvatarProvider* avatarProvider)
    : QObject(parent),
      m_db(db),
      m_settingsManager(settingsManager),
      m_avatarProvider(avatarProvider),
      m_settingsService(db) {
  connect(&MainSignals::instance(), &MainSignals::avatarDeleted, this,
          &AccountManager::onAvatarDeleted);
  connect(m_db, &DatabaseManager::databaseOpened, this,
          [this](const QString&) { reloadIdentityCache(); });
}

void AccountManager::reloadIdentityCache() {
  publicKeyRaw_ = m_settingsService.loadBlobValue(QStringLiteral("public_key"));
  privateKeyRaw_ =
      m_settingsService.loadBlobValue(QStringLiteral("private_key"));
  inviteSecretRaw_ =
      m_settingsService.loadBlobValue(QStringLiteral("invite_secret"));

  if (publicKeyRaw_.isEmpty()) {
    publicKey_.clear();
    return;
  }

  std::vector<uint8_t> pubkeyVec(publicKeyRaw_.begin(), publicKeyRaw_.end());
  publicKey_ = QString::fromStdString(CoreCrypto::to_hex(pubkeyVec));
}

bool AccountManager::create(const QString& mnemonicphrase) {
  if (!CoreCrypto::mnemonicphrase_check(mnemonicphrase.toStdString())) {
    return false;
  }

  std::vector<uint8_t> seed =
      CoreCrypto::mnemonicphrase_to_seed(mnemonicphrase.toStdString());
  CoreCrypto::SignKeyPair keypair = CoreCrypto::seed_to_keypair(seed);

  m_settingsService.saveBlobValue(
      QStringLiteral("public_key"),
      QByteArray(reinterpret_cast<const char*>(keypair.pubkey.data()),
                 static_cast<int>(keypair.pubkey.size())));
  m_settingsService.saveBlobValue(
      QStringLiteral("private_key"),
      QByteArray(reinterpret_cast<const char*>(keypair.privkey.data()),
                 static_cast<int>(keypair.privkey.size())));

  // Generate the invite secret up front, alongside the identity, so it is
  // present and stable from the very first use (QR, sessions) with no lazy
  // generation racing between paths.
  const std::vector<uint8_t> invite =
      CoreCrypto::random(inviteutils::kInviteSecretSize);
  const QByteArray inviteQ(reinterpret_cast<const char*>(invite.data()),
                           static_cast<int>(invite.size()));
  m_settingsService.saveBlobValue(QStringLiteral("invite_secret"), inviteQ);

  m_db->commitTransaction();

  inviteSecretRaw_ = inviteQ;
  publicKeyRaw_ =
      QByteArray(reinterpret_cast<const char*>(keypair.pubkey.data()),
                 static_cast<int>(keypair.pubkey.size()));
  privateKeyRaw_ =
      QByteArray(reinterpret_cast<const char*>(keypair.privkey.data()),
                 static_cast<int>(keypair.privkey.size()));
  std::vector<uint8_t> pubkeyVec(keypair.pubkey.begin(), keypair.pubkey.end());
  publicKey_ = QString::fromStdString(CoreCrypto::to_hex(pubkeyVec));

  return true;
}

QString AccountManager::getPublicKey() {
  if (publicKey_.isEmpty() && m_db->isOpen()) {
    reloadIdentityCache();
  }
  return publicKey_;
}

QByteArray AccountManager::getPublicKeyRaw() {
  if (publicKeyRaw_.isEmpty() && m_db->isOpen()) {
    reloadIdentityCache();
  }
  return publicKeyRaw_;
}

QByteArray AccountManager::getPrivateKey() {
  if (privateKeyRaw_.isEmpty() && m_db->isOpen()) {
    reloadIdentityCache();
  }
  return privateKeyRaw_;
}

QByteArray AccountManager::getOrCreateInviteSecret() {
  if (inviteSecretRaw_.size() == inviteutils::kInviteSecretSize) {
    return inviteSecretRaw_;
  }

  if (m_db != nullptr && m_db->isOpen()) {
    const QByteArray stored =
        m_settingsService.loadBlobValue(QStringLiteral("invite_secret"));
    if (stored.size() == inviteutils::kInviteSecretSize) {
      inviteSecretRaw_ = stored;
      return stored;
    }
  }

  // Fallback for accounts created before invites existed: generate and persist
  // synchronously (blobvalues), so this exact value is what both the QR and the
  // receiver-side ratchet seed read from now on.
  const std::vector<uint8_t> fresh =
      CoreCrypto::random(inviteutils::kInviteSecretSize);
  const QByteArray secret(reinterpret_cast<const char*>(fresh.data()),
                          static_cast<int>(fresh.size()));
  m_settingsService.saveBlobValue(QStringLiteral("invite_secret"), secret);
  m_db->commitTransaction();
  inviteSecretRaw_ = secret;
  return secret;
}

void AccountManager::rotateInviteSecret() {
  const std::vector<uint8_t> fresh =
      CoreCrypto::random(inviteutils::kInviteSecretSize);
  const QByteArray secret(reinterpret_cast<const char*>(fresh.data()),
                          static_cast<int>(fresh.size()));
  m_settingsService.saveBlobValue(QStringLiteral("invite_secret"), secret);
  m_db->commitTransaction();
  inviteSecretRaw_ = secret;
  emit inviteChanged();
}

QString AccountManager::contactShareString() {
  const QString server =
      m_settingsManager != nullptr
          ? m_settingsManager->getTextSetting(QStringLiteral("serverAddress"))
          : QString();
  return inviteutils::buildContactString(getPublicKeyRaw(),
                                         getOrCreateInviteSecret(), server);
}

QVariantMap AccountManager::parseContactString(const QString& text) const {
  const inviteutils::ParsedContact parsed =
      inviteutils::parseContactString(text);
  QVariantMap map;
  map[QStringLiteral("valid")] = parsed.valid;
  map[QStringLiteral("pubKey")] = parsed.pubkeyHex;
  map[QStringLiteral("server")] = parsed.server;
  map[QStringLiteral("invite")] =
      QString::fromLatin1(parsed.inviteSecret.toHex());
  return map;
}

prekeys::PreKey AccountManager::generatePreKey(bool _commit) {
  qDebug() << "[AccountManager] Generating new prekey";
  prekeys::PreKey prekey;

  pqdh::KeyPairs keypairs = pqdh::generate_keypairs();

  prekey.dh_pub = keypairs.dh_pub;
  prekey.pq_pub = keypairs.pq_pk;

  QByteArray ed25519_pubkey = this->getPublicKeyRaw();
  std::vector<uint8_t> ed25519_pubkey_vec(ed25519_pubkey.begin(),
                                          ed25519_pubkey.end());
  prekey.Ed25519_pub = ed25519_pubkey_vec;

  std::vector<uint8_t> to_id;

  to_id.reserve(prekey.dh_pub.size() + prekey.pq_pub.size() +
                prekey.Ed25519_pub.size());
  to_id.insert(to_id.end(), prekey.dh_pub.begin(), prekey.dh_pub.end());
  to_id.insert(to_id.end(), prekey.pq_pub.begin(), prekey.pq_pub.end());
  to_id.insert(to_id.end(), prekey.Ed25519_pub.begin(),
               prekey.Ed25519_pub.end());

  prekey.id = CoreCrypto::sha256(to_id);

  std::vector<uint8_t> to_sign;

  to_sign.reserve(prekey.id.size() + prekey.dh_pub.size() +
                  prekey.pq_pub.size() + prekey.Ed25519_pub.size());
  to_sign.insert(to_sign.end(), prekey.id.begin(), prekey.id.end());
  to_sign.insert(to_sign.end(), prekey.dh_pub.begin(), prekey.dh_pub.end());
  to_sign.insert(to_sign.end(), prekey.pq_pub.begin(), prekey.pq_pub.end());
  to_sign.insert(to_sign.end(), prekey.Ed25519_pub.begin(),
                 prekey.Ed25519_pub.end());

  QByteArray privkey = AccountManager::getPrivateKey();
  std::vector<uint8_t> privkeyVec(privkey.begin(), privkey.end());

  std::vector<uint8_t> signature = CoreCrypto::sign(privkeyVec, to_sign);

  prekey.signature = signature;

  QVariantList params;
  params << QByteArray(reinterpret_cast<const char*>(prekey.id.data()),
                       static_cast<int>(prekey.id.size()))
         << QByteArray(reinterpret_cast<const char*>(prekey.dh_pub.data()),
                       static_cast<int>(prekey.dh_pub.size()))
         << QByteArray(reinterpret_cast<const char*>(prekey.pq_pub.data()),
                       static_cast<int>(prekey.pq_pub.size()))
         << QByteArray(reinterpret_cast<const char*>(prekey.Ed25519_pub.data()),
                       static_cast<int>(prekey.Ed25519_pub.size()))
         << QByteArray(reinterpret_cast<const char*>(prekey.signature.data()),
                       static_cast<int>(prekey.signature.size()))
         << QByteArray(reinterpret_cast<const char*>(keypairs.dh_priv.data()),
                       static_cast<int>(keypairs.dh_priv.size()))
         << QByteArray(reinterpret_cast<const char*>(keypairs.pq_sk.data()),
                       static_cast<int>(keypairs.pq_sk.size()));

  m_db->execute(
      "INSERT INTO prekeys (id, dh_pub, pq_pub, ed25519_pub, signature, "
      "dh_priv, pq_priv) VALUES (?, ?, ?, ?, ?, ?, ?)",
      params);

  if (_commit) {
    m_db->commitTransaction();
  }

  return prekey;
}

void AccountManager::setContactAvatar(const QString& serverId,
                                      const QString& contactPubKey,
                                      const QByteArray& avatarBlob) {
  if (m_avatarProvider == nullptr) {
    return;
  }

  m_avatarProvider->addAvatar(contactAvatarId(serverId, contactPubKey),
                              avatarBlob);
}

void AccountManager::clearContactAvatar(const QString& serverId,
                                        const QString& contactPubKey) {
  if (m_avatarProvider == nullptr) {
    return;
  }

  m_avatarProvider->removeAvatar(contactAvatarId(serverId, contactPubKey));
}

void AccountManager::selectAndSaveAvatar() {
  filepicker::pickOpenFile(
      this, tr("Choose avatar"),
      QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
      QStringLiteral("Images (*.png *.jpg *.jpeg)"),
      [this](const QString& filePath) {
        if (filePath.isEmpty()) {
          return;
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
          return;
        }

        const QByteArray rawBytes = file.readAll();

        // Avatars are broadcast in full to every contact on every profile save
        // (buildOwnAccountInfoJson embeds them base64, which inflates ~33%), so
        // an un-resized multi-MB photo becomes a multi-MB message per contact
        // and jams offline sync. Downscale to a small thumbnail and re-encode as
        // JPEG before storing/sending. Falls back to the raw bytes only if the
        // file isn't a decodable image.
        QByteArray blob = rawBytes;
        QImage image;
        if (image.loadFromData(rawBytes)) {
          constexpr int kMaxDimension = 512;
          if (image.width() > kMaxDimension || image.height() > kMaxDimension) {
            image = image.scaled(kMaxDimension, kMaxDimension,
                                  Qt::KeepAspectRatio, Qt::SmoothTransformation);
          }
          // JPEG has no alpha channel; flatten transparency onto white so a
          // transparent PNG avatar doesn't come out on a black background.
          if (image.hasAlphaChannel()) {
            QImage opaque(image.size(), QImage::Format_RGB32);
            opaque.fill(Qt::white);
            QPainter painter(&opaque);
            painter.drawImage(0, 0, image);
            painter.end();
            image = opaque;
          }
          QByteArray encoded;
          QBuffer buffer(&encoded);
          buffer.open(QIODevice::WriteOnly);
          if (image.save(&buffer, "JPEG", 85) && !encoded.isEmpty()) {
            blob = encoded;
          }
          buffer.close();
        } else {
          qWarning() << "[AccountManager] avatar not a decodable image, storing"
                        " raw bytes:"
                     << rawBytes.size();
        }

        qWarning() << "[AccountManager] avatar stored, raw" << rawBytes.size()
                   << "-> final" << blob.size() << "bytes";

        this->m_settingsManager->setBlobSetting("avatar", blob);
        m_avatarProvider->addAvatar("0", blob);
        emit avatarChanged();
      });
}

void AccountManager::loadOwnAvatar() {
  QByteArray blob =
      this->m_settingsManager->getBlobSetting("avatar", QByteArray());
  if (!blob.isEmpty()) {
    qDebug() << "[AccountManager] Loaded own avatar from database, size:"
             << blob.size();
    m_avatarProvider->addAvatar(
        "0",
        blob);  // "0" is the current user ID, and the current user is 0
    emit avatarChanged();
  } else {
    qDebug() << "[AccountManager] No avatar found in database.";
  }
}