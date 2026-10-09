#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>
#include <cstring>

#include "doubleratchet.h"

// Helpers for the invite secret that gates new E2E sessions.
//
// The invite is a 128-bit random secret carried inside the contact QR/string.
// It never goes over the wire: both sides mix it into the initial Double
// Ratchet root key (instead of the old all-zero key), so a session_request
// from someone who does not hold the current invite produces a diverged
// ratchet whose first message simply fails AEAD and is dropped. That is the
// whole gate — no server-side checks, the server never sees the secret.
//
// The shareable contact string is:
//   base64url( pubkey(32) || invite(16) || server(utf8) || checksum(4) )
// where the checksum is a truncated SHA-256 over the preceding bytes. base64
// is for copy-paste convenience only, not protection; the checksum catches
// corrupted/truncated pastes (a flipped character would otherwise silently
// yield a contact that can never connect).
namespace inviteutils {

inline constexpr int kInviteSecretSize = 16;  // 128-bit invite secret
inline constexpr int kPubKeySize = 32;        // ed25519 identity key
inline constexpr int kChecksumSize = 4;       // truncated SHA-256 of the blob

// Domain-separated derivation of the initial Double Ratchet root key from the
// invite secret. Both sides must call this with the same secret for the
// ratchet chains to converge.
inline dr::Key32 deriveRootKey(const QByteArray& inviteSecret) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(QByteArrayLiteral("patronus-invite-root-v1"));
  hash.addData(inviteSecret);
  const QByteArray digest = hash.result();  // 32 bytes for SHA-256
  dr::Key32 key{};
  std::memcpy(key.data(), digest.constData(),
              std::min(key.size(), static_cast<size_t>(digest.size())));
  return key;
}

inline QByteArray contactChecksum(const QByteArray& blob) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(QByteArrayLiteral("patronus-contact-checksum-v1"));
  hash.addData(blob);
  return hash.result().left(kChecksumSize);
}

// Builds the shareable contact string. Returns an empty string when any part
// is missing/malformed (e.g. account not created yet).
inline QString buildContactString(const QByteArray& pubkeyRaw,
                                  const QByteArray& inviteSecret,
                                  const QString& server) {
  const QByteArray serverUtf8 = server.trimmed().toUtf8();
  if (pubkeyRaw.size() != kPubKeySize ||
      inviteSecret.size() != kInviteSecretSize || serverUtf8.isEmpty()) {
    return QString();
  }

  QByteArray blob;
  blob.reserve(kPubKeySize + kInviteSecretSize + serverUtf8.size() +
               kChecksumSize);
  blob += pubkeyRaw;
  blob += inviteSecret;
  blob += serverUtf8;
  blob += contactChecksum(blob);

  return QString::fromLatin1(blob.toBase64(QByteArray::Base64UrlEncoding |
                                           QByteArray::OmitTrailingEquals));
}

struct ParsedContact {
  bool valid = false;
  QString pubkeyHex;        // 64 hex chars
  QByteArray inviteSecret;  // kInviteSecretSize raw bytes
  QString server;
};

// Parses a pasted/scanned contact string. Decoding is lenient (stray
// whitespace or padding from messengers is tolerated); the checksum is the
// actual integrity check.
inline ParsedContact parseContactString(const QString& text) {
  ParsedContact result;

  const QByteArray decoded = QByteArray::fromBase64(
      text.trimmed().toUtf8(), QByteArray::Base64UrlEncoding);
  constexpr int kMinSize =
      kPubKeySize + kInviteSecretSize + 1 + kChecksumSize;  // server >= 1 byte
  if (decoded.size() < kMinSize) {
    return result;
  }

  const QByteArray blob = decoded.left(decoded.size() - kChecksumSize);
  if (decoded.right(kChecksumSize) != contactChecksum(blob)) {
    return result;
  }

  result.pubkeyHex = QString::fromLatin1(blob.left(kPubKeySize).toHex());
  result.inviteSecret = blob.mid(kPubKeySize, kInviteSecretSize);
  result.server =
      QString::fromUtf8(blob.mid(kPubKeySize + kInviteSecretSize)).trimmed();
  result.valid = !result.server.isEmpty();
  return result;
}

}  // namespace inviteutils
