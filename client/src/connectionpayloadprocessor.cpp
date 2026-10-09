#include "connectionpayloadprocessor.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QObject>

#include "accountmanager.h"
#include "connectionstorage.h"
#include "contactsmodel.h"
#include "coreutils.h"
#include "doubleratchet.h"
#include "filemessageutils.h"
#include "inviteutils.h"
#include "mainsignals.h"
#include "messagemodel.h"
#include "namestylevalidator.h"
#include "pqdh.h"
#include "prekeys.h"
#include "wireprotocol.h"

namespace {

// Reads the reply-quote fields from an incoming chat message. The wire carries
// `reply_author_is_sender` (was the quoted message written by the peer sending
// this one); from the receiver's side the quoted message is "own" exactly when
// the sender did NOT author it. Missing reply_to means "not a reply".
ReplyInfo parseIncomingReply(const nlohmann::json& msg) {
  ReplyInfo reply;
  reply.replyTo = filemessage::readUInt64(msg, "reply_to");
  if (reply.replyTo == 0) {
    return reply;
  }
  reply.preview =
      QString::fromStdString(msg.value("reply_preview", std::string()));
  reply.kind = QString::fromStdString(msg.value("reply_kind", std::string()));
  bool authorIsSender = false;
  if (msg.contains("reply_author_is_sender")) {
    const auto& value = msg.at("reply_author_is_sender");
    if (value.is_boolean()) {
      authorIsSender = value.get<bool>();
    } else if (value.is_number()) {
      authorIsSender = value.get<double>() != 0.0;
    }
  }
  reply.isOwn = !authorIsSender;
  return reply;
}

}  // namespace

ConnectionPayloadProcessor::ConnectionPayloadProcessor(
    ConnectionStorage* storage, AccountManager* accountManager,
    ContactsModel* contactsModel, MessageModel* messageModel,
    Callbacks callbacks)
    : m_storage(storage),
      m_accountManager(accountManager),
      m_contactsModel(contactsModel),
      m_messageModel(messageModel),
      m_callbacks(std::move(callbacks)),
      m_nameStyleValidator(std::make_unique<NameStyleValidator>()) {}

ConnectionPayloadProcessor::~ConnectionPayloadProcessor() = default;

quint64 ConnectionPayloadProcessor::parseMessageId(
    const nlohmann::json& msg) const {
  if (!msg.contains("message_id")) {
    return 0;
  }

  if (msg["message_id"].is_string()) {
    return QString::fromStdString(msg["message_id"].get<std::string>())
        .toULongLong();
  }
  if (msg["message_id"].is_number_unsigned()) {
    return msg["message_id"].get<quint64>();
  }
  if (msg["message_id"].is_number_integer()) {
    return static_cast<quint64>(msg["message_id"].get<qint64>());
  }

  return 0;
}

quint64 ConnectionPayloadProcessor::parseMessageTimestamp(
    const nlohmann::json& msg) const {
  if (!msg.contains("timestamp")) {
    return static_cast<quint64>(
        QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
  }

  if (msg["timestamp"].is_string()) {
    return QString::fromStdString(msg["timestamp"].get<std::string>())
        .toULongLong();
  }
  if (msg["timestamp"].is_number_unsigned()) {
    return msg["timestamp"].get<quint64>();
  }
  if (msg["timestamp"].is_number_integer()) {
    return static_cast<quint64>(msg["timestamp"].get<qint64>());
  }

  return static_cast<quint64>(
      QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
}

void ConnectionPayloadProcessor::handleForwardFrame(const QString& serverId,
                                                    const QByteArray& data) {
  // Defense-in-depth: the dispatcher already length-checks FORWARD frames, but
  // this handler also runs on reconstructed FORWARD_LIVE frames, so it must not
  // trust the caller. Reject anything too short for [type][fromKey][format]
  // before the fixed-offset reads below.
  if (data.size() < wire::kForwardMinSize) {
    qWarning() << "[ConnectionManager] [FORWARD] frame too short from"
               << serverId << "size=" << data.size();
    return;
  }
  try {
    // messageData is the frame with the leading type byte stripped, so offsets
    // here are body-relative: [fromKey(kPubKeySize)][format(kMsgFormatSize)]...
    std::vector<uint8_t> messageData(data.begin() + wire::kTypeSize,
                                     data.end());
    std::vector<uint8_t> from(messageData.begin(),
                              messageData.begin() + wire::kPubKeySize);
    const QString fromPubKey =
        QString::fromStdString(coreutils::bytes_to_hex(from));
    const uint8_t responseFormat =
        static_cast<uint8_t>(messageData[wire::kPubKeySize]);

    if (responseFormat ==
        static_cast<uint8_t>(dr::EncryptedMessageFormat::NOT_ENCRYPTED_JSON)) {
      std::vector<uint8_t> plaintext(
          messageData.begin() + wire::kPubKeySize + wire::kMsgFormatSize,
          messageData.end());
      nlohmann::json msg = nlohmann::json::parse(
          std::string(plaintext.begin(), plaintext.end()), nullptr, false);
      if (msg.is_discarded()) {
        qWarning() << "[ConnectionManager] [FORWARD] [NOT_ENCRYPTED_JSON] Failed "
                      "to parse plaintext JSON from"
                   << fromPubKey;
        return;
      }

      if (!(msg.contains("type") &&
            msg["type"].get<std::string>() == "session_request")) {
        qWarning() << "[ConnectionManager] [FORWARD] [NOT_ENCRYPTED_JSON] "
                      "Unknown message type in plaintext JSON from"
                   << fromPubKey;
        return;
      }

      std::vector<uint8_t> prekeyId;
      std::vector<uint8_t> dhPub;
      std::vector<uint8_t> pqPub;
      std::vector<uint8_t> edPub;
      std::vector<uint8_t> encapsulatedCiphertext;
      std::vector<uint8_t> signature;

      try {
        prekeyId = coreutils::hex_to_bytes(msg["prekey_id"].get<std::string>());
        dhPub = coreutils::hex_to_bytes(msg["dh_pub"].get<std::string>());
        pqPub = coreutils::hex_to_bytes(msg["pq_pub"].get<std::string>());
        edPub = coreutils::hex_to_bytes(msg["ed_pub"].get<std::string>());
        encapsulatedCiphertext = coreutils::hex_to_bytes(
            msg["encapsulated_ciphertext"].get<std::string>());
        signature = coreutils::hex_to_bytes(msg["signature"].get<std::string>());
      } catch (const std::exception& e) {
        qWarning()
            << "[ConnectionManager] [FORWARD] [NOT_ENCRYPTED_JSON] "
               "[session_request] Error parsing session request fields from"
            << fromPubKey << ":" << e.what();
        return;
      }

      QByteArray myEdPub = m_accountManager->getPublicKeyRaw();
      std::vector<uint8_t> myPubVec(myEdPub.begin(), myEdPub.end());
      std::vector<uint8_t> toVerify;
      toVerify.insert(toVerify.end(), prekeyId.begin(), prekeyId.end());
      toVerify.insert(toVerify.end(), dhPub.begin(), dhPub.end());
      toVerify.insert(toVerify.end(), pqPub.begin(), pqPub.end());
      toVerify.insert(toVerify.end(), edPub.begin(), edPub.end());
      toVerify.insert(toVerify.end(), myPubVec.begin(), myPubVec.end());
      toVerify.insert(toVerify.end(), encapsulatedCiphertext.begin(),
                      encapsulatedCiphertext.end());

      if (!CoreCrypto::verify(edPub, toVerify, signature)) {
        qWarning()
            << "[ConnectionManager] [FORWARD] [NOT_ENCRYPTED_JSON] "
               "[session_request] Invalid signature in session request from"
            << fromPubKey;
        return;
      }

      dr::DoubleRatchet doubleRatchet;
      if (!m_storage->hasPrekey(prekeyId)) {
        qWarning()
            << "[ConnectionManager] [FORWARD] [NOT_ENCRYPTED_JSON] "
               "[session_request] Prekey ID not found for session request from"
            << fromPubKey;
        return;
      }

      if (!m_callbacks.getOwnInviteSecret) {
        qWarning() << "[ConnectionManager] [FORWARD] [NOT_ENCRYPTED_JSON] "
                      "[session_request] No invite secret provider, dropping "
                      "session request from"
                   << fromPubKey;
        return;
      }

      // Seed the ratchet with our invite secret instead of the old zero key:
      // an initiator who does not hold the current invite ends up with a
      // diverged ratchet whose messages never decrypt, so nothing below ever
      // materializes for them.
      const QByteArray ownInvite = m_callbacks.getOwnInviteSecret();
      const dr::Key32 initialRootKey = inviteutils::deriveRootKey(ownInvite);

      pqdh::KeyPairs pqdhKeys = m_storage->loadPrekey(prekeyId);
      doubleRatchet.pq_init(initialRootKey, pqdhKeys, dhPub,
                            encapsulatedCiphertext, false);
      spdlog::set_level(spdlog::level::debug);
      doubleRatchet.debug_print();

      // Deferred materialization: store the session as a pending row only. The
      // contact appears in the list when the first message from this peer
      // decrypts successfully (see the decrypt path below).
      m_storage->storePendingContactSession(serverId, fromPubKey,
                                            doubleRatchet);
      return;
    }

    std::vector<uint8_t> ciphertext(
        messageData.begin() + wire::kPubKeySize + wire::kMsgFormatSize,
        messageData.end());
    dr::EncryptedMessage message = dr::EncryptedMessage::deserialize(ciphertext);
    dr::DoubleRatchet doubleRatchet =
        m_storage->loadDoubleRatchet(serverId, fromPubKey);
    bool ok = false;
    std::vector<uint8_t> decrypted = doubleRatchet.decrypt(message, &ok);
    if (!ok) {
      qWarning() << "[ConnectionManager] [FORWARD] AEAD failed, dropping frame, "
                    "ratchet left unchanged from"
                 << fromPubKey;
      return;
    }
    m_storage->saveDoubleRatchet(serverId, fromPubKey, doubleRatchet);

    // First successful decrypt from a peer we don't list yet: they proved
    // knowledge of our invite secret (the session was seeded with it), so
    // promote the pending session to a visible contact before dispatching the
    // message (handlers like accountInfo update the contact record). They
    // scanned our QR, so also admit them to the server whitelist — the server
    // ignores this unless we are ourselves a member.
    if (!m_contactsModel->hasContact(serverId, fromPubKey)) {
      m_storage->materializePendingContact(serverId, fromPubKey);
      m_contactsModel->addAnonymousContact(
          serverId, fromPubKey,
          QString::fromStdString(doubleRatchet.get_session_key_fingerprint()),
          QObject::tr("Session established. You can start messaging."));
      if (m_callbacks.enrollMember) {
        m_callbacks.enrollMember(serverId, fromPubKey);
      }
    }

    if (responseFormat ==
        static_cast<uint8_t>(dr::EncryptedMessageFormat::JSON)) {
      nlohmann::json msg = nlohmann::json::parse(
          std::string(decrypted.begin(), decrypted.end()), nullptr, false);
      if (msg.is_discarded()) {
        qWarning() << "[ConnectionManager] [FORWARD] [EncryptedMessageFormat] "
                      "[JSON] Failed to parse decrypted JSON from"
                   << fromPubKey;
        return;
      }
      handleJsonMessage(serverId, fromPubKey, msg);
    } else if (responseFormat ==
               static_cast<uint8_t>(dr::EncryptedMessageFormat::BINARY)) {
      handleBinaryMessage(
          serverId, fromPubKey,
          QByteArray(reinterpret_cast<const char*>(decrypted.data()),
                     static_cast<int>(decrypted.size())));
    } else {
      qWarning() << "[ConnectionManager] [FORWARD] [EncryptedMessageFormat] "
                    "Unknown response format from"
                 << fromPubKey << ":" << static_cast<int>(responseFormat);
    }
  } catch (const std::exception& e) {
    qWarning() << "[ConnectionManager] [FORWARD] dropped malformed frame from"
               << serverId << "err=" << e.what();
  } catch (...) {
    qWarning() << "[ConnectionManager] [FORWARD] dropped malformed frame "
                  "(unknown exc) from"
               << serverId;
  }
}

void ConnectionPayloadProcessor::handleAccountInfoMessage(
    const QString& serverId, const QString& fromPubKey,
    const nlohmann::json& msg) {
  if (!msg.contains("firstName") || !msg.contains("lastName") ||
      !msg.contains("aboutMe")) {
    qWarning() << "[ConnectionManager] [json_message_handler] accountInfo "
                  "message missing fields from"
               << fromPubKey;
    return;
  }

  QVariantMap updates;
  updates["firstName"] =
      QString::fromStdString(msg.value("firstName", std::string()));
  updates["lastName"] =
      QString::fromStdString(msg.value("lastName", std::string()));
  updates["aboutMe"] =
      QString::fromStdString(msg.value("aboutMe", std::string()));

  // The name style is an opaque CSS-subset string. Validate it here with the
  // same strict whitelist parser the QML layer uses at render time
  // (NameStyleParser.js): a peer must not be able to push anything the local
  // parser wouldn't accept. Anything that fails validation is dropped to an
  // empty style rather than stored — we accept nothing we can't vouch for. The
  // length is capped first so a huge blob can't reach the parser.
  QString nameStyle =
      QString::fromStdString(msg.value("nameStyle", std::string()));
  constexpr int kMaxNameStyleLength = 4000;
  if (nameStyle.length() > kMaxNameStyleLength) {
    nameStyle.truncate(kMaxNameStyleLength);
  }
  if (!m_nameStyleValidator->isValid(nameStyle)) {
    if (!nameStyle.isEmpty()) {
      qWarning() << "[ConnectionManager] [json_message_handler] rejecting "
                    "invalid nameStyle from"
                 << fromPubKey;
    }
    nameStyle.clear();
  }
  updates["nameStyle"] = nameStyle;

  QByteArray avatarBlob;
  if (msg.contains("avatarBlob")) {
    const std::string encoded = msg["avatarBlob"].get<std::string>();
    avatarBlob = QByteArray::fromBase64(QByteArray::fromStdString(encoded));
    if (avatarBlob.isEmpty() && !encoded.empty()) {
      qWarning() << "[ConnectionManager] [json_message_handler] Failed to "
                    "decode avatarBlob from"
                 << fromPubKey;
    }
  }
  updates["avatarBlob"] = avatarBlob;

  if (!avatarBlob.isEmpty()) {
    m_accountManager->setContactAvatar(serverId, fromPubKey, avatarBlob);
    updates["avatarSource"] =
        m_accountManager->getContactAvatarSource(serverId, fromPubKey);
  } else {
    m_accountManager->clearContactAvatar(serverId, fromPubKey);
    updates["avatarSource"] = QString();
  }

  if (m_callbacks.contactAvatarChanged) {
    m_callbacks.contactAvatarChanged(serverId, fromPubKey);
  }
  m_contactsModel->updateContact(serverId, fromPubKey, updates);

  if (msg.contains("is_response") && msg["is_response"].get<bool>()) {
    return;
  }

  if (m_callbacks.buildOwnAccountInfoJson && m_callbacks.sendJson) {
    m_callbacks.sendJson(serverId, fromPubKey,
                         m_callbacks.buildOwnAccountInfoJson(true));
  }
}

void ConnectionPayloadProcessor::handleDeleteMessageJson(
    const QString& serverId, const QString& fromPubKey,
    const nlohmann::json& msg) {
  const quint64 mid = parseMessageId(msg);
  MainSignals::instance().emitMessageDeleteReceived(mid, serverId, fromPubKey);
}

void ConnectionPayloadProcessor::handleTextMessageJson(
    const QString& serverId, const QString& fromPubKey,
    const nlohmann::json& msg) {
  const QString qText =
      QString::fromStdString(msg.value("text", std::string()));
  const quint64 messageId = parseMessageId(msg);
  const quint64 timestamp = parseMessageTimestamp(msg);
  const ReplyInfo reply = parseIncomingReply(msg);

  // Disappearing-message TTL chosen by the sender at send time, carried with
  // the message so our copy expires in lockstep with theirs regardless of
  // whether our local contact timer has caught up to a recent set_timer. -1
  // means the field is absent (legacy peer) -> fall back to our contact timer.
  int timerSeconds = -1;
  if (msg.contains("timer") &&
      (msg["timer"].is_number_integer() || msg["timer"].is_number_unsigned())) {
    timerSeconds = msg["timer"].get<int>();
  }

  MainSignals::instance().emitE2eTextMessageReceived(
      serverId, fromPubKey, qText, messageId, timestamp, reply.replyTo,
      reply.preview, reply.kind, reply.isOwn, timerSeconds);
  if (m_callbacks.textMessageReceived) {
    m_callbacks.textMessageReceived(serverId, fromPubKey, qText, messageId);
  }
  if (m_callbacks.sendReceivedSignalForMessage) {
    m_callbacks.sendReceivedSignalForMessage(serverId, fromPubKey, messageId);
  }
  m_contactsModel->setLastMessageAt(serverId, fromPubKey, qText, timestamp);
}

void ConnectionPayloadProcessor::handleMediaMessageJson(
    const QString& serverId, const QString& fromPubKey,
    const nlohmann::json& msg, const QString& type) {
  FileMessageData fileData;
  fileData.fileId = QString::fromStdString(msg.value("file_id", std::string()));
  fileData.fileName =
      QString::fromStdString(msg.value("file_name", std::string()));
  fileData.fileSize = filemessage::readUInt64(msg, "file_size");
  fileData.fileSha256 =
      QString::fromStdString(msg.value("file_sha256", std::string()));
  fileData.fileKey =
      QString::fromStdString(msg.value("file_key", std::string()));
  fileData.encryption =
      QString::fromStdString(msg.value("encryption", std::string()));
  fileData.blobSize = filemessage::readUInt64(msg, "blob_size");
  fileData.mediaType =
      QString::fromStdString(msg.value("media_type", std::string()));
  if (type == QStringLiteral("audio") && fileData.mediaType.isEmpty()) {
    fileData.mediaType = QStringLiteral("audio");
  }
  fileData.mimeType =
      QString::fromStdString(msg.value("mime_type", std::string()));
  fileData.durationMs = filemessage::readUInt64(msg, "duration_ms");
  fileData.waveform = filemessage::readIntList(msg, "waveform");

  const quint64 messageId = parseMessageId(msg);
  const quint64 timestamp = parseMessageTimestamp(msg);
  if (fileData.fileId.isEmpty() || fileData.fileName.isEmpty() ||
      fileData.fileKey.isEmpty() || messageId == 0) {
    qWarning() << "[ConnectionManager] [json_message_handler] media message "
                  "missing fields from"
               << fromPubKey;
    return;
  }

  const QString previewText = filemessage::previewText(fileData);
  const QByteArray content = filemessage::serialize(fileData);
  const auto messageType = type == QStringLiteral("audio")
                               ? MessageModel::MESSAGE_TYPE::AUDIO
                               : MessageModel::MESSAGE_TYPE::FILE;
  const ReplyInfo reply = parseIncomingReply(msg);

  m_storage->addTypedMessage(messageId, serverId, fromPubKey, previewText,
                             messageType, timestamp, false, false, false, false,
                             content, reply);
  if (messageType == MessageModel::MESSAGE_TYPE::AUDIO) {
    m_messageModel->receiveAudioMessage(messageId, serverId, fromPubKey,
                                        previewText, content, timestamp, reply);
  } else {
    m_messageModel->receiveFileMessage(messageId, serverId, fromPubKey,
                                       previewText, content, timestamp, reply);
  }

  if (m_callbacks.textMessageReceived) {
    m_callbacks.textMessageReceived(serverId, fromPubKey, previewText,
                                    messageId);
  }
  if (m_callbacks.sendReceivedSignalForMessage) {
    m_callbacks.sendReceivedSignalForMessage(serverId, fromPubKey, messageId);
  }
  MainSignals::instance().emitMediaMessageReceived(serverId, fromPubKey,
                                                   messageId, false);
  m_contactsModel->setLastMessageAt(serverId, fromPubKey, previewText,
                                    timestamp);
}

void ConnectionPayloadProcessor::handleAlbumMessageJson(
    const QString& serverId, const QString& fromPubKey,
    const nlohmann::json& msg) {
  const quint64 messageId = parseMessageId(msg);
  const quint64 timestamp = parseMessageTimestamp(msg);
  if (messageId == 0) {
    qWarning() << "[ConnectionManager] [json_message_handler] album message "
                  "missing id from"
               << fromPubKey;
    return;
  }

  AlbumMessageData album;
  album.caption = QString::fromStdString(msg.value("caption", std::string()));
  if (msg.contains("items") && msg.at("items").is_array()) {
    for (const auto& itemJson : msg.at("items")) {
      if (!itemJson.is_object()) {
        continue;
      }
      FileMessageData item;
      // The wire carries no local_path, so it stays empty until downloaded.
      filemessage::fromJson(itemJson, &item);
      if (item.fileId.isEmpty() || item.fileKey.isEmpty()) {
        continue;
      }
      album.items.append(item);
    }
  }
  if (album.items.isEmpty()) {
    qWarning() << "[ConnectionManager] [json_message_handler] album message "
                  "had no valid items from"
               << fromPubKey;
    return;
  }

  const ReplyInfo reply = parseIncomingReply(msg);
  const QByteArray content = filemessage::serializeAlbum(album);
  const QString previewText =
      album.caption.isEmpty() ? QStringLiteral("Album") : album.caption;

  m_storage->addTypedMessage(
      messageId, serverId, fromPubKey, previewText,
      static_cast<int>(MessageModel::MESSAGE_TYPE::ALBUM), timestamp, false,
      false, false, false, content, reply);
  m_messageModel->receiveAlbumMessage(messageId, serverId, fromPubKey,
                                      album.caption, content, timestamp, reply);

  if (m_callbacks.textMessageReceived) {
    m_callbacks.textMessageReceived(serverId, fromPubKey, previewText,
                                    messageId);
  }
  if (m_callbacks.sendReceivedSignalForMessage) {
    m_callbacks.sendReceivedSignalForMessage(serverId, fromPubKey, messageId);
  }
  MainSignals::instance().emitMediaMessageReceived(serverId, fromPubKey,
                                                   messageId, true);
  m_contactsModel->setLastMessageAt(serverId, fromPubKey, previewText,
                                    timestamp);
}

void ConnectionPayloadProcessor::handleCallAcceptMessage(
    const QString& serverId, const QString& fromPubKey,
    const nlohmann::json& msg) {
  if (!msg.contains("dh_pub") || !msg.contains("id") ||
      !msg.contains("callId") || !msg["callId"].is_number_unsigned()) {
    qWarning() << "[ConnectionManager] [json_message_handler] call_accept "
                  "message missing fields from"
               << fromPubKey;
    return;
  }

  std::vector<uint8_t> dhPub;
  std::vector<uint8_t> id;
  try {
    dhPub = coreutils::hex_to_bytes(msg["dh_pub"].get<std::string>());
    id = coreutils::hex_to_bytes(msg["id"].get<std::string>());
  } catch (const std::exception& e) {
    qWarning() << "[ConnectionManager] [json_message_handler] call_accept "
                  "malformed hex fields from"
               << fromPubKey << ":" << e.what();
    return;
  }
  const quint64 callId = static_cast<quint64>(msg["callId"].get<uint64_t>());
  const QString idStr = QString::fromStdString(coreutils::bytes_to_hex(id));

  auto contactOpt = m_contactsModel->getCallContactInfo(serverId, fromPubKey);
  if (!contactOpt) {
    qWarning() << "[ConnectionManager] [json_message_handler] call_accept "
                  "received from unknown contact"
               << fromPubKey;
    return;
  }

  const ContactCallInfo contact = contactOpt.value();
  if (!m_storage->hasPrekey(idStr)) {
    qWarning() << "[ConnectionManager] [json_message_handler] call_accept "
                  "received with unknown prekey id from"
               << fromPubKey;
    return;
  }

  const pqdh::KeyPairs prekey = m_storage->loadPrekey(idStr);
  if (m_callbacks.setCallSessionState) {
    m_callbacks.setCallSessionState(
        CoreCrypto::derive_dh_keys(false, prekey.dh_pub, prekey.dh_priv, dhPub),
        contact.server, contact.pubkeyFingerprint);
  }

  const quint64 timestamp = parseMessageTimestamp(msg);
  m_messageModel->addCallStatusMessageIfCurrentChat(
      serverId, fromPubKey, callId, QObject::tr("Accepted Call"), true);
  m_storage->addCallMessageAsync(callId, serverId, fromPubKey,
                                 QObject::tr("Accepted Call"), timestamp, true,
                                 true, true, true);
  m_contactsModel->setLastMessageAt(serverId, fromPubKey,
                                    QObject::tr("Accepted Call"), timestamp);

  if (m_callbacks.callAcceptReceived) {
    m_callbacks.callAcceptReceived();
  }
}

void ConnectionPayloadProcessor::handleCallRequestMessage(
    const QString& serverId, const QString& fromPubKey,
    const nlohmann::json& msg) {
  if (!msg.contains("dh_pub") || !msg.contains("id") ||
      !msg.contains("timestamp") || !msg.contains("callId") ||
      !msg["callId"].is_number_unsigned()) {
    qWarning() << "[ConnectionManager] [json_message_handler] call_request "
                  "message missing fields from"
               << fromPubKey;
    return;
  }

  std::vector<uint8_t> dhPub;
  std::vector<uint8_t> id;
  try {
    dhPub = coreutils::hex_to_bytes(msg["dh_pub"].get<std::string>());
    id = coreutils::hex_to_bytes(msg["id"].get<std::string>());
  } catch (const std::exception& e) {
    qWarning() << "[ConnectionManager] [json_message_handler] call_request "
                  "malformed hex fields from"
               << fromPubKey << ":" << e.what();
    return;
  }
  const QString dhPubStr =
      QString::fromStdString(coreutils::bytes_to_hex(dhPub));
  const QString idStr = QString::fromStdString(coreutils::bytes_to_hex(id));
  const uint64_t callId = msg["callId"].get<uint64_t>();

  auto contactOpt = m_contactsModel->getCallContactInfo(serverId, fromPubKey);
  if (!contactOpt) {
    qWarning() << "[ConnectionManager] [json_message_handler] call_request "
                  "received from unknown contact"
               << fromPubKey;
    return;
  }

  const ContactCallInfo contact = contactOpt.value();
  if (m_callbacks.callRequestReceived) {
    m_callbacks.callRequestReceived(contact.firstName, contact.avatarSource,
                                    contact.server, fromPubKey, dhPubStr, idStr,
                                    callId);
  }
}

void ConnectionPayloadProcessor::handleJsonMessage(const QString& serverId,
                                                   const QString& fromPubKey,
                                                   const nlohmann::json& msg) {
  if (msg.contains("type")) {
    const QString type = QString::fromStdString(msg["type"].get<std::string>());
    if (type == QStringLiteral("accountInfo")) {
      handleAccountInfoMessage(serverId, fromPubKey, msg);
    } else if (type == QStringLiteral("session_remove")) {
      MainSignals::instance().emitSessionRemoveReceived(serverId, fromPubKey);
    } else if (type == QStringLiteral("delete_message")) {
      handleDeleteMessageJson(serverId, fromPubKey, msg);
    } else if (type == QStringLiteral("text")) {
      handleTextMessageJson(serverId, fromPubKey, msg);
    } else if (type == QStringLiteral("file") ||
               type == QStringLiteral("audio")) {
      handleMediaMessageJson(serverId, fromPubKey, msg, type);
    } else if (type == QStringLiteral("album")) {
      handleAlbumMessageJson(serverId, fromPubKey, msg);
    } else if (type == QStringLiteral("call_discard")) {
      if (!msg.contains("callId") || !msg["callId"].is_number_unsigned()) {
        qWarning() << "[ConnectionManager] [json_message_handler] "
                      "call_discard message missing callId from"
                   << fromPubKey;
      } else if (m_callbacks.callDiscardReceived) {
        // Optional teardown reason: "busy" means the peer was already on another
        // call. Absent on normal hang-ups and on older clients (treated as "").
        QString reason;
        if (msg.contains("reason") && msg["reason"].is_string()) {
          reason = QString::fromStdString(msg["reason"].get<std::string>());
        }
        m_callbacks.callDiscardReceived(
            serverId, fromPubKey,
            static_cast<quint64>(msg["callId"].get<uint64_t>()), reason);
      }
    } else if (type == QStringLiteral("call_cancel")) {
      if (!msg.contains("callId") || !msg["callId"].is_number_unsigned()) {
        qWarning() << "[ConnectionManager] [json_message_handler] "
                      "call_cancel message missing callId from"
                   << fromPubKey;
      } else if (m_callbacks.callCancelReceived) {
        m_callbacks.callCancelReceived(
            serverId, fromPubKey,
            static_cast<quint64>(msg["callId"].get<uint64_t>()));
      }
    } else if (type == QStringLiteral("call_accept")) {
      handleCallAcceptMessage(serverId, fromPubKey, msg);
    } else if (type == QStringLiteral("call_request")) {
      handleCallRequestMessage(serverId, fromPubKey, msg);
    } else if (type == QStringLiteral("identity_revoked")) {
      // A contact announced their own key is compromised. fromPubKey IS the
      // revoked identity, so verify the certificate is a valid ed25519 signature
      // by that key over the domain-separated revocation message before acting —
      // otherwise a peer could grief a third party's key.
      if (!msg.contains("signature") || !msg["signature"].is_string()) {
        qWarning() << "[ConnectionManager] [json_message_handler] "
                      "identity_revoked missing signature from"
                   << fromPubKey;
      } else {
        const std::vector<uint8_t> identity =
            coreutils::hex_to_bytes(fromPubKey.toStdString());
        const std::vector<uint8_t> signature =
            coreutils::hex_to_bytes(msg["signature"].get<std::string>());
        static const std::string kRevocationContext =
            "PATRONUS_KEY_REVOCATION_v1";
        std::vector<uint8_t> signedMessage(kRevocationContext.begin(),
                                           kRevocationContext.end());
        signedMessage.insert(signedMessage.end(), identity.begin(),
                             identity.end());
        if (CoreCrypto::verify(identity, signedMessage, signature)) {
          MainSignals::instance().emitIdentityRevocationReceived(serverId,
                                                                 fromPubKey);
        } else {
          qWarning() << "[ConnectionManager] [json_message_handler] "
                        "identity_revoked with INVALID signature from"
                     << fromPubKey;
        }
      }
    } else if (type == QStringLiteral("clear_history")) {
      MainSignals::instance().emitClearHistoryReceived(serverId, fromPubKey);
    } else if (type == QStringLiteral("set_timer")) {
      if (!msg.contains("seconds") || !msg["seconds"].is_number_integer()) {
        qWarning() << "[ConnectionManager] [json_message_handler] set_timer "
                      "message missing seconds from"
                   << fromPubKey;
      } else {
        MainSignals::instance().emitSetTimerReceived(
            serverId, fromPubKey, msg["seconds"].get<int>());
      }
    } else {
      qWarning() << "[ConnectionManager] [json_message_handler] Unknown JSON "
                    "message type from"
                 << fromPubKey << ":" << type;
    }
  } else {
    qWarning() << "[ConnectionManager] [json_message_handler] JSON message "
                  "missing 'type' field from"
               << fromPubKey;
  }
}

void ConnectionPayloadProcessor::handleBinaryMessage(const QString& serverId,
                                                     const QString& fromPubKey,
                                                     const QByteArray& data) {
  Q_UNUSED(serverId);
  Q_UNUSED(fromPubKey);
  Q_UNUSED(data);
}