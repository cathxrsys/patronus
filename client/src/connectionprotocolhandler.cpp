#include "connectionprotocolhandler.h"

#include <corecrypto.h>

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QtConcurrent>

#include "accountmanager.h"
#include "connectionmanager.h"
#include "contactsmodel.h"
#include "coreutils.h"
#include "mainsignals.h"
#include "messagemodel.h"
#include "prekeys.h"
#include "wireprotocol.h"

namespace {

// Rejects a frame shorter than the fixed fields its handler reads. Returns true
// (and logs) when the frame is too short so the caller can bail out instead of
// reading out of bounds. Centralises the bounds check that guards every
// fixed-offset parse below.
bool frameTooShort(const QByteArray& data, int minSize, const char* tag,
                   const QString& serverId) {
  if (data.size() < minSize) {
    qWarning() << "[ConnectionProtocolHandler]" << tag
               << "frame too short from" << serverId
               << "size=" << data.size() << "min=" << minSize;
    return true;
  }
  return false;
}

// Bounds for chunked-frame reassembly: refuse a transfer larger than the wire
// message cap, and cap how many may be in flight at once, so a buggy or hostile
// peer can't exhaust memory by announcing huge or endless transfers.
constexpr int kMaxChunkedTransferBytes = 10 * 1024 * 1024;  // server MaxWSMessageSize
constexpr int kMaxConcurrentTransfers = 16;
// A chunk frame is [type][4-byte transferId][...]; data starts after both.
constexpr int kChunkHeaderSize = 1 + 4;
constexpr int kChunkBeginSize = 1 + 4 + 4;  // + 4-byte total length

}  // namespace

ConnectionProtocolHandler::ConnectionProtocolHandler(
    AccountManager* accountManager, ContactsModel* contactsModel,
    MessageModel* messageModel, Callbacks callbacks)
    : m_accountManager(accountManager),
      m_contactsModel(contactsModel),
      m_messageModel(messageModel),
      m_callbacks(std::move(callbacks)) {}

uint32_t ConnectionProtocolHandler::parseRequestId(const QByteArray& data,
                                                   int offset) const {
  return static_cast<uint32_t>(static_cast<uint8_t>(data[offset])) << 24 |
         static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 1])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 2])) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 3]));
}

quint64 ConnectionProtocolHandler::parseBinaryMessageId(const QByteArray& data,
                                                        int offset) const {
  quint64 messageId = 0;
  for (int i = 0; i < 8; ++i) {
    messageId = (messageId << 8) | static_cast<uint8_t>(data[offset + i]);
  }
  return messageId;
}

QString ConnectionProtocolHandler::parseHexKey(const QByteArray& data,
                                               int offset, int size) const {
  const QByteArray raw = data.mid(offset, size);
  const std::vector<uint8_t> bytes(raw.begin(), raw.end());
  return QString::fromStdString(coreutils::bytes_to_hex(bytes));
}

void ConnectionProtocolHandler::handleProtocolMismatchFrame(
    const QString& serverId, const QByteArray& data) const {
  QString errorText = QCoreApplication::translate(
      "ConnectionManager", "Incompatible protocol version");

  const QByteArray payload = data.mid(wire::kTypeSize);
  const std::string payloadJson(payload.constData(),
                                static_cast<size_t>(payload.size()));
  nlohmann::json json = nlohmann::json::parse(payloadJson, nullptr, false);
  if (!json.is_discarded()) {
    const QString message =
        json.contains("message")
            ? QString::fromStdString(json["message"].get<std::string>())
            : QString();
    const QString serverVersion =
        json.contains("server_protocol_version")
            ? QString::fromStdString(
                  json["server_protocol_version"].get<std::string>())
            : QString();
    const QString clientVersion =
        json.contains("client_protocol_version")
            ? QString::fromStdString(
                  json["client_protocol_version"].get<std::string>())
            : QString();

    if (!message.isEmpty()) {
      errorText = message;
    }

    if (!serverVersion.isEmpty() || !clientVersion.isEmpty()) {
      errorText +=
          QStringLiteral(" (%1: %2, %3: %4)")
              .arg(QCoreApplication::translate("ConnectionManager", "server"),
                   serverVersion.isEmpty() ? QCoreApplication::translate(
                                                 "ConnectionManager", "unknown")
                                           : serverVersion,
                   QCoreApplication::translate("ConnectionManager", "client"),
                   clientVersion.isEmpty() ? QCoreApplication::translate(
                                                 "ConnectionManager", "missing")
                                           : clientVersion);
    }
  }

  if (m_callbacks.protocolVersionMismatch) {
    m_callbacks.protocolVersionMismatch(serverId, errorText);
  }
}

void ConnectionProtocolHandler::handleReceivedSignalFrame(
    const QString& serverId, const QByteArray& data) const {
  if (frameTooShort(data, wire::kReceivedSignalMin, "[RECEIVED_SIGNAL]",
                    serverId)) {
    return;
  }
  const QString fromPubKey =
      parseHexKey(data, wire::kFromKeyOffset, wire::kPubKeySize);
  const quint64 messageId =
      parseBinaryMessageId(data, wire::kPayloadAfterFromKey);

  if (m_messageModel != nullptr) {
    m_messageModel->updateMessageDeliveredStatus(serverId, fromPubKey,
                                                 messageId);
  }
  MainSignals::instance().emitUpdateMessageDeliveredStatus(serverId, fromPubKey,
                                                           messageId);
}

void ConnectionProtocolHandler::handlePrekeyFrame(
    const QString& serverId, const QByteArray& data) const {
  if (frameTooShort(data, wire::kBodyAfterRequestId, "[PREKEY]", serverId)) {
    return;
  }
  const uint32_t requestId = parseRequestId(data);
  const QByteArray prekeyData = data.mid(wire::kBodyAfterRequestId);
  const std::string prekeyJson(prekeyData.constData(),
                               static_cast<size_t>(prekeyData.size()));

  nlohmann::json prekeyJsonObj =
      nlohmann::json::parse(prekeyJson, nullptr, false);
  if (prekeyJsonObj.is_discarded()) {
    qWarning()
        << "[ConnectionManager] [PREKEY] Failed to parse prekey JSON from"
        << serverId;
    return;
  }

  if (prekeyJsonObj.contains("error_code") &&
      prekeyJsonObj["error_code"].get<int>() != 0) {
    QJSValueList args;
    args << QJSValue(prekeyJsonObj["error_code"].get<int>());
    args << QJSValue(QString::fromStdString(
        prekeyJsonObj["error_message"].get<std::string>()));
    if (m_callbacks.invokeCallback) {
      m_callbacks.invokeCallback(requestId, args,
                                 QStringLiteral("[ConnectionManager] [PREKEY]"),
                                 serverId, QString());
    }
    return;
  }

  prekeys::PreKey prekey = prekeys::json_to_prekey(prekeyJson);
  QJSValueList args;
  args << QJSValue(QString::fromUtf8(
      QByteArray(reinterpret_cast<const char*>(prekey.id.data()),
                 static_cast<int>(prekey.id.size()))
          .toHex()));
  args << QJSValue(QString::fromUtf8(
      QByteArray(reinterpret_cast<const char*>(prekey.dh_pub.data()),
                 static_cast<int>(prekey.dh_pub.size()))
          .toHex()));
  args << QJSValue(QString::fromUtf8(
      QByteArray(reinterpret_cast<const char*>(prekey.pq_pub.data()),
                 static_cast<int>(prekey.pq_pub.size()))
          .toHex()));
  args << QJSValue(QString::fromUtf8(
      QByteArray(reinterpret_cast<const char*>(prekey.Ed25519_pub.data()),
                 static_cast<int>(prekey.Ed25519_pub.size()))
          .toHex()));
  args << QJSValue(QString::fromUtf8(
      QByteArray(reinterpret_cast<const char*>(prekey.signature.data()),
                 static_cast<int>(prekey.signature.size()))
          .toHex()));
  if (m_callbacks.invokeCallback) {
    m_callbacks.invokeCallback(requestId, args,
                               QStringLiteral("[ConnectionManager] [PREKEY]"),
                               serverId, QString());
  }
}

void ConnectionProtocolHandler::handleOnlineStatusFrame(
    const QString& serverId, const QByteArray& data) const {
  if (frameTooShort(data, wire::kOnlineKeyOffset, "[ONLINE_STATUS]",
                    serverId)) {
    return;
  }
  const uint32_t requestId = parseRequestId(data);
  const bool isOnline =
      static_cast<bool>(static_cast<uint8_t>(data[wire::kBodyAfterRequestId]));
  const QString contactPubKey = parseHexKey(data, wire::kOnlineKeyOffset,
                                            data.size() - wire::kOnlineKeyOffset);

  if (m_contactsModel != nullptr) {
    m_contactsModel->setContactOnline(serverId, contactPubKey, isOnline);
  }

  if (m_callbacks.invokeCallback) {
    QJSValueList args;
    args << isOnline;
    m_callbacks.invokeCallback(
        requestId, args, QStringLiteral("[ConnectionManager] [ONLINE_STATUS]"),
        serverId, QString());
  }
}

void ConnectionProtocolHandler::handleRequestStateFrame(
    const QString& serverId, const QByteArray& data,
    const QString& state) const {
  if (!m_callbacks.invokeCallback) {
    return;
  }

  const uint32_t requestId = parseRequestId(data);
  QJSValueList args;
  args << QJSValue(state);
  m_callbacks.invokeCallback(requestId, args,
                             QStringLiteral("[ConnectionManager] [") +
                                 state.toUpper() + QStringLiteral("]"),
                             serverId, state);
}

void ConnectionProtocolHandler::handleFcmTokenReceivedFrame(
    const QString& serverId, const QByteArray& data) const {
  if (!m_callbacks.fcmTokenRegistered ||
      data.size() < wire::kBodyAfterRequestId) {
    return;
  }

  const uint32_t requestId = parseRequestId(data);
  qDebug() << "[ConnectionProtocolHandler] FCM token registration ack received"
           << "serverId=" << serverId << "requestId=" << requestId;
  m_callbacks.fcmTokenRegistered(serverId, requestId);
}

void ConnectionProtocolHandler::handleFcmNotUsedFrame(
    const QString& serverId, const QByteArray& data) const {
  if (!m_callbacks.fcmNotUsed || data.size() < wire::kBodyAfterRequestId) {
    return;
  }

  const uint32_t requestId = parseRequestId(data);
  qDebug() << "[ConnectionProtocolHandler] FCM not used response received"
           << "serverId=" << serverId << "requestId=" << requestId;
  m_callbacks.fcmNotUsed(serverId, requestId);
}

void ConnectionProtocolHandler::handleEnrollAckFrame(
    const QString& serverId, const QByteArray& data) const {
  if (!m_callbacks.enrollAcked || data.size() < wire::kBodyAfterRequestId) {
    return;
  }

  const uint32_t requestId = parseRequestId(data);
  qDebug() << "[ConnectionProtocolHandler] Enroll ack received"
           << "serverId=" << serverId << "requestId=" << requestId;
  m_callbacks.enrollAcked(serverId, requestId);
}

void ConnectionProtocolHandler::handleChunkBegin(
    const QString& serverId, const QByteArray& data,
    ConnectionServerRuntimeState* runtimeState) const {
  if (frameTooShort(data, kChunkBeginSize, "[CHUNK_BEGIN]", serverId)) {
    return;
  }
  const quint32 transferId = parseRequestId(data, 1);
  const quint32 total = parseRequestId(data, 1 + 4);
  if (total > static_cast<quint32>(kMaxChunkedTransferBytes)) {
    qWarning() << "[ConnectionProtocolHandler] [CHUNK_BEGIN] transfer too large,"
                  " dropping"
               << total << "bytes from" << serverId;
    return;
  }
  if (!runtimeState->chunkTransfers.contains(transferId) &&
      runtimeState->chunkTransfers.size() >= kMaxConcurrentTransfers) {
    qWarning() << "[ConnectionProtocolHandler] [CHUNK_BEGIN] too many concurrent"
                  " transfers, dropping"
               << transferId << "from" << serverId;
    return;
  }
  ChunkReassembly reassembly;
  reassembly.total = total;
  reassembly.buffer.reserve(static_cast<int>(total));
  runtimeState->chunkTransfers.insert(transferId, reassembly);
}

void ConnectionProtocolHandler::handleChunkData(
    const QString& serverId, const QByteArray& data,
    ConnectionServerRuntimeState* runtimeState) const {
  if (frameTooShort(data, kChunkHeaderSize, "[CHUNK_DATA]", serverId)) {
    return;
  }
  const quint32 transferId = parseRequestId(data, 1);
  auto it = runtimeState->chunkTransfers.find(transferId);
  if (it == runtimeState->chunkTransfers.end()) {
    return;  // no matching begin (dropped for size/count) — ignore silently
  }
  it->buffer.append(data.constData() + kChunkHeaderSize,
                    data.size() - kChunkHeaderSize);
  if (static_cast<quint32>(it->buffer.size()) > it->total) {
    qWarning() << "[ConnectionProtocolHandler] [CHUNK_DATA] overflow past"
                  " announced size, dropping transfer"
               << transferId << "from" << serverId;
    runtimeState->chunkTransfers.erase(it);
  }
}

void ConnectionProtocolHandler::handleChunkEnd(
    const QString& serverId, const QByteArray& data,
    ConnectionServerRuntimeState* runtimeState,
    QString* prekeysSyncServerId) const {
  if (frameTooShort(data, kChunkHeaderSize, "[CHUNK_END]", serverId)) {
    return;
  }
  const quint32 transferId = parseRequestId(data, 1);
  auto it = runtimeState->chunkTransfers.find(transferId);
  if (it == runtimeState->chunkTransfers.end()) {
    return;
  }
  const QByteArray assembled = it->buffer;
  const quint32 expected = it->total;
  runtimeState->chunkTransfers.erase(it);
  if (static_cast<quint32>(assembled.size()) != expected) {
    qWarning() << "[ConnectionProtocolHandler] [CHUNK_END] incomplete transfer"
               << transferId << "got" << assembled.size() << "expected"
               << expected << "from" << serverId;
    return;
  }
  if (assembled.isEmpty()) {
    return;
  }
  // Dispatch the reassembled frame exactly as if it had arrived whole. Its first
  // byte is the original response type (never itself a chunk type), so this
  // cannot recurse endlessly.
  processAuthedBinaryFrame(serverId, assembled, runtimeState,
                           prekeysSyncServerId);
}

void ConnectionProtocolHandler::processAuthedBinaryFrame(
    const QString& serverId, const QByteArray& data,
    ConnectionServerRuntimeState* runtimeState,
    QString* prekeysSyncServerId) const {
  if (runtimeState == nullptr) {
    return;
  }

  try {
    const auto responseType = static_cast<ConnectionManager::ResponseType>(
        static_cast<uint8_t>(data[0]));

    switch (responseType) {
      case ConnectionManager::ResponseType::PROTOCOL_MISMATCH:
        handleProtocolMismatchFrame(serverId, data);
        break;
      case ConnectionManager::ResponseType::FCM_TOKEN_RECEIVED:
        handleFcmTokenReceivedFrame(serverId, data);
        break;
      case ConnectionManager::ResponseType::FCM_NOT_USED:
        handleFcmNotUsedFrame(serverId, data);
        break;
      case ConnectionManager::ResponseType::AUDIO_FRAME:
        if (frameTooShort(data, wire::kPayloadAfterFromKey, "[AUDIO_FRAME]",
                          serverId)) {
          break;
        }
        if (m_callbacks.audioFrameReceived) {
          m_callbacks.audioFrameReceived(data.mid(wire::kPayloadAfterFromKey));
        }
        break;
      case ConnectionManager::ResponseType::READED_SIGNAL:
        if (frameTooShort(data, wire::kPayloadAfterFromKey, "[READED_SIGNAL]",
                          serverId)) {
          break;
        }
        MainSignals::instance().emitAllOwnMessagesReaded(
            serverId, parseHexKey(data, wire::kFromKeyOffset, wire::kPubKeySize));
        break;
      case ConnectionManager::ResponseType::SESSION_REMOVED_SIGNAL:
        if (frameTooShort(data, wire::kPayloadAfterFromKey,
                          "[SESSION_REMOVED_SIGNAL]", serverId)) {
          break;
        }
        MainSignals::instance().emitSessionRemoveReceived(
            serverId, parseHexKey(data, wire::kFromKeyOffset, wire::kPubKeySize));
        break;
      case ConnectionManager::ResponseType::ENROLL_ACK:
        handleEnrollAckFrame(serverId, data);
        break;
      case ConnectionManager::ResponseType::IDENTITY_REVOKED:
        // The server hands this back when we try to reach a revoked identity
        // (e.g. we reply to someone the attacker messaged as us). The frame is
        // [type][revoked identity key]; no signature travels with it, but the
        // server only issues it for a tombstone it verified at revocation time.
        if (frameTooShort(data, wire::kPayloadAfterFromKey, "[IDENTITY_REVOKED]",
                          serverId)) {
          break;
        }
        MainSignals::instance().emitIdentityRevocationReceived(
            serverId, parseHexKey(data, wire::kFromKeyOffset, wire::kPubKeySize));
        break;
      case ConnectionManager::ResponseType::RECEIVED_SIGNAL:
        handleReceivedSignalFrame(serverId, data);
        break;
      case ConnectionManager::ResponseType::AUTH_SUCCESS:
        runtimeState->accessToken = QString::fromUtf8(data.mid(wire::kTypeSize));
        qWarning() << "[ConnectionProtocolHandler] AUTH_SUCCESS received"
                 << "serverId=" << serverId
                 << "accessTokenLen=" << runtimeState->accessToken.size();
        if (!runtimeState->authCompleted) {
          runtimeState->authCompleted = true;
          runtimeState->syncing = false;
          if (m_callbacks.authChanged) {
            m_callbacks.authChanged(serverId, true);
          }
        }
        if (prekeysSyncServerId != nullptr) {
          *prekeysSyncServerId = serverId;
        }
        break;
      case ConnectionManager::ResponseType::PREKEYS_COUNT:
        if (data.size() >= wire::kPrekeysCountMin) {
          runtimeState->prekeysSyncInFlight = false;
          const int targetPrekeys = m_callbacks.prekeyTargetCount
                                        ? m_callbacks.prekeyTargetCount()
                                        : 100;
          const int currentPrekeys =
              static_cast<uint8_t>(data[wire::kBodyAfterRequestId]);
          const int prekeysRemaining = qMax(0, targetPrekeys - currentPrekeys);
          if (prekeysRemaining > 0 && m_callbacks.sendRequest &&
              m_accountManager != nullptr) {
            if (m_callbacks.prekeyGenerationStateChanged) {
              m_callbacks.prekeyGenerationStateChanged(
                  serverId, true, prekeysRemaining, targetPrekeys);
            }
            auto accountManager = m_accountManager;
            auto currentServerId = serverId;
            int prekeysToGenerate = prekeysRemaining;
            auto notifyStateChanged = m_callbacks.prekeyGenerationStateChanged;
            (void)QtConcurrent::run([accountManager, currentServerId,
                                     prekeysToGenerate, targetPrekeys,
                                     sendRequest = m_callbacks.sendRequest,
                                     notifyStateChanged]() {
              std::string preKeysJsonArray = "[";
              for (int index = 0; index < prekeysToGenerate; ++index) {
                prekeys::PreKey prekey = accountManager->generatePreKey(false);
                preKeysJsonArray += prekeys::prekey_to_json(prekey) + ",";
              }
              if (preKeysJsonArray.back() == ',') {
                preKeysJsonArray.pop_back();
              }
              preKeysJsonArray += "]";

              const QByteArray prekeysData(
                  preKeysJsonArray.c_str(),
                  static_cast<int>(preKeysJsonArray.size()));
              QObject* application = QCoreApplication::instance();
              if (application == nullptr) {
                if (notifyStateChanged) {
                  notifyStateChanged(currentServerId, false, prekeysToGenerate,
                                     targetPrekeys);
                }
                return;
              }

              QMetaObject::invokeMethod(accountManager, "commit",
                                        Qt::QueuedConnection);
              QMetaObject::invokeMethod(
                  application,
                  [sendRequest, currentServerId, prekeysData, notifyStateChanged,
                   prekeysToGenerate, targetPrekeys]() {
                    sendRequest(currentServerId, 2, prekeysData);
                    if (notifyStateChanged) {
                      notifyStateChanged(currentServerId, false,
                                         prekeysToGenerate, targetPrekeys);
                    }
                  },
                  Qt::QueuedConnection);
            });
          }
        } else {
          runtimeState->prekeysSyncInFlight = false;
        }
        break;
      case ConnectionManager::ResponseType::PREKEY:
        handlePrekeyFrame(serverId, data);
        break;
      case ConnectionManager::ResponseType::FORWARD:
        if (frameTooShort(data, wire::kForwardMinSize, "[FORWARD]", serverId)) {
          break;
        }
        if (m_callbacks.handleForwardFrame) {
          m_callbacks.handleForwardFrame(serverId, data);
        }
        break;
      case ConnectionManager::ResponseType::FORWARD_LIVE: {
        if (frameTooShort(data, wire::kForwardLiveHeader, "[FORWARD_LIVE]",
                          serverId)) {
          break;
        }
        const quint64 dbId =
            parseBinaryMessageId(data, wire::kPayloadAfterFromKey);
        QByteArray forwardFrame;
        forwardFrame.reserve(wire::kPayloadAfterFromKey +
                             (data.size() - wire::kForwardLiveHeader));
        forwardFrame.append(
            static_cast<char>(static_cast<uint8_t>(ConnectionManager::ResponseType::FORWARD)));
        forwardFrame.append(data.mid(wire::kTypeSize, wire::kPubKeySize));
        forwardFrame.append(data.mid(wire::kForwardLiveHeader));
        if (m_callbacks.handleForwardFrame) {
          m_callbacks.handleForwardFrame(serverId, forwardFrame);
        }
        if (m_callbacks.sendMessageAck) {
          m_callbacks.sendMessageAck(serverId, dbId);
        }
        break;
      }
      case ConnectionManager::ResponseType::ONLINE_STATUS:
        handleOnlineStatusFrame(serverId, data);
        break;
      case ConnectionManager::ResponseType::DELIVERED:
        handleRequestStateFrame(serverId, data, QStringLiteral("delivered"));
        break;
      case ConnectionManager::ResponseType::RECEIVED:
        handleRequestStateFrame(serverId, data, QStringLiteral("received"));
        break;
      case ConnectionManager::ResponseType::SYNC_END:
        runtimeState->syncing = false;
        if (m_callbacks.syncEnded) {
          m_callbacks.syncEnded(serverId);
        }
        break;
      case ConnectionManager::ResponseType::SYNC_START:
        runtimeState->syncing = true;
        if (m_callbacks.syncStarted) {
          m_callbacks.syncStarted(serverId);
        }
        break;
      case ConnectionManager::ResponseType::AUTHENTICATED:
        // Handshake confirmed before sync begins: flip to "authenticated /
        // synchronizing" so the UI stops showing a stuck "authenticating". The
        // client still holds its outbound storm until AUTH_SUCCESS (post-sync).
        if (!runtimeState->authenticated) {
          runtimeState->authenticated = true;
          qWarning() << "[ConnectionProtocolHandler] AUTHENTICATED received for"
                     << serverId;
          if (m_callbacks.authenticated) {
            m_callbacks.authenticated(serverId);
          }
        }
        break;
      case ConnectionManager::ResponseType::CHUNK_BEGIN:
        handleChunkBegin(serverId, data, runtimeState);
        break;
      case ConnectionManager::ResponseType::CHUNK_DATA:
        handleChunkData(serverId, data, runtimeState);
        break;
      case ConnectionManager::ResponseType::CHUNK_END:
        handleChunkEnd(serverId, data, runtimeState, prekeysSyncServerId);
        break;
      default:
        qWarning() << "[ConnectionManager] [RECEIVED] Unknown response type from"
                   << serverId << ':' << static_cast<int>(responseType);
        break;
    }
  } catch (const std::exception& e) {
    qWarning() << "[ConnectionProtocolHandler] dropped malformed authed frame from"
               << serverId
               << "type=" << (data.isEmpty() ? -1 : int(uchar(data[0])))
               << "err=" << e.what();
  } catch (...) {
    qWarning() << "[ConnectionProtocolHandler] dropped malformed authed frame (unknown exc) from"
               << serverId;
  }
}

void ConnectionProtocolHandler::handleBinaryFrame(
    const QString& serverId, const QByteArray& data,
    ConnectionServerRuntimeState* runtimeState, QByteArray* responseToSend,
    QString* prekeysSyncServerId) const {
  if (runtimeState == nullptr || responseToSend == nullptr ||
      m_accountManager == nullptr) {
    return;
  }

  if (!runtimeState->authResponseSent) {
    if (!data.isEmpty() &&
        static_cast<ConnectionManager::ResponseType>(static_cast<uint8_t>(
            data[0])) == ConnectionManager::ResponseType::PROTOCOL_MISMATCH &&
        data.size() != wire::kChallengeSize) {
      handleProtocolMismatchFrame(serverId, data);
      return;
    }

    const QByteArray challenge = data;
    const std::vector<uint8_t> challengeVec(challenge.begin(), challenge.end());

    const QByteArray pubkey = m_accountManager->getPublicKeyRaw();
    const std::vector<uint8_t> pubkeyVec(pubkey.begin(), pubkey.end());
    const QByteArray pubkeyArr(reinterpret_cast<const char*>(pubkeyVec.data()),
                               static_cast<int>(pubkeyVec.size()));

    const QByteArray privkey = m_accountManager->getPrivateKey();
    const std::vector<uint8_t> privkeyVec(privkey.begin(), privkey.end());
    const std::vector<uint8_t> signature =
        CoreCrypto::sign(privkeyVec, challengeVec);
    const QByteArray signatureArr(
        reinterpret_cast<const char*>(signature.data()),
        static_cast<int>(signature.size()));

    responseToSend->clear();
    responseToSend->append(pubkeyArr);
    responseToSend->append(signatureArr);
    runtimeState->authResponseSent = true;
    return;
  }

  processAuthedBinaryFrame(serverId, data, runtimeState, prekeysSyncServerId);
}