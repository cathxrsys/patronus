#ifndef CONNECTIONPROTOCOLHANDLER_H
#define CONNECTIONPROTOCOLHANDLER_H

#include <QByteArray>
#include <QHash>
#include <QJSValue>
#include <QString>
#include <functional>

class AccountManager;
class ContactsModel;
class MessageModel;

// A large frame that arrived split across CHUNK_BEGIN/DATA/END frames, being
// reassembled. Keyed by transferId in the runtime state below.
struct ChunkReassembly {
  quint32 total = 0;
  QByteArray buffer;
};

struct ConnectionServerRuntimeState {
  bool authResponseSent = false;
  // Handshake confirmed by the server (ResponseAuthenticated), before sync.
  // Distinct from authCompleted: the connection is authenticated but still
  // synchronizing, so the UI can show a real "synchronizing" step.
  bool authenticated = false;
  // Offline sync in progress (between SYNC_START and SYNC_END).
  bool syncing = false;
  bool authCompleted = false;
  bool prekeysSyncInFlight = false;
  QString accessToken;
  // In-flight chunked transfers keyed by transferId. Cleared wholesale on
  // disconnect (the runtime state is reset), so stale buffers never leak across
  // reconnects even though transferIds restart from 0 on the server.
  QHash<quint32, ChunkReassembly> chunkTransfers;
};

class ConnectionProtocolHandler {
 public:
  struct Callbacks {
    std::function<void(const QString&, bool)> authChanged;
    std::function<void(const QString&, const QString&)> protocolVersionMismatch;
    std::function<void(uint32_t, const QJSValueList&, const QString&,
                       const QString&, const QString&)>
        invokeCallback;
    std::function<void(const QString&, uint32_t)> fcmTokenRegistered;
    std::function<void(const QString&, uint32_t)> fcmNotUsed;
    std::function<void(const QString&, uint32_t)> enrollAcked;
    std::function<uint32_t(const QString&, uint8_t, const QByteArray&)>
        sendRequest;
    std::function<int()> prekeyTargetCount;
    std::function<void(const QString&, bool, int, int)>
        prekeyGenerationStateChanged;
    std::function<void(const QString&, const QByteArray&)> handleForwardFrame;
    std::function<void(const QString&, quint64)> sendMessageAck;
    std::function<void(const QByteArray&)> audioFrameReceived;
    std::function<void(const QString&)> authenticated;
    std::function<void(const QString&)> syncStarted;
    std::function<void(const QString&)> syncEnded;
  };

  ConnectionProtocolHandler(AccountManager* accountManager,
                            ContactsModel* contactsModel,
                            MessageModel* messageModel, Callbacks callbacks);

  void handleBinaryFrame(const QString& serverId, const QByteArray& data,
                         ConnectionServerRuntimeState* runtimeState,
                         QByteArray* responseToSend,
                         QString* prekeysSyncServerId) const;

 private:
  void handleProtocolMismatchFrame(const QString& serverId,
                                   const QByteArray& data) const;
  uint32_t parseRequestId(const QByteArray& data, int offset = 1) const;
  quint64 parseBinaryMessageId(const QByteArray& data, int offset) const;
  QString parseHexKey(const QByteArray& data, int offset, int size) const;
  void handleFcmTokenReceivedFrame(const QString& serverId,
                                   const QByteArray& data) const;
  void handleFcmNotUsedFrame(const QString& serverId,
                             const QByteArray& data) const;
  void handleEnrollAckFrame(const QString& serverId,
                            const QByteArray& data) const;
  void handleReceivedSignalFrame(const QString& serverId,
                                 const QByteArray& data) const;
  void handlePrekeyFrame(const QString& serverId, const QByteArray& data) const;
  void handleOnlineStatusFrame(const QString& serverId,
                               const QByteArray& data) const;
  void handleRequestStateFrame(const QString& serverId, const QByteArray& data,
                               const QString& state) const;
  void handleChunkBegin(const QString& serverId, const QByteArray& data,
                        ConnectionServerRuntimeState* runtimeState) const;
  void handleChunkData(const QString& serverId, const QByteArray& data,
                       ConnectionServerRuntimeState* runtimeState) const;
  void handleChunkEnd(const QString& serverId, const QByteArray& data,
                      ConnectionServerRuntimeState* runtimeState,
                      QString* prekeysSyncServerId) const;
  void processAuthedBinaryFrame(const QString& serverId, const QByteArray& data,
                                ConnectionServerRuntimeState* runtimeState,
                                QString* prekeysSyncServerId) const;

  AccountManager* m_accountManager = nullptr;
  ContactsModel* m_contactsModel = nullptr;
  MessageModel* m_messageModel = nullptr;
  Callbacks m_callbacks;
};

#endif