#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

#include <QHash>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <memory>

#include "accountmanager.h"
#include "connectionprotocolhandler.h"
#include "connectionstorage.h"
#include "connectiontransport.h"
#include "contactsmodel.h"
#include "doubleratchet.h"
#include "mainsignals.h"
#include "messagemodel.h"
#include "pq.h"
#include "pqdh.h"
#include "prekeys.h"
#include "settingsmanager.h"

class ConnectionManager : public QObject {
  Q_OBJECT

 public:
  enum class ConnectionStatus {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Error
  };
  Q_ENUM(ConnectionStatus)

  enum class ResponseType : uint8_t {  // server response types
    RECEIVED = 0,   // receipt acknowledgment
    DELIVERED = 1,  // delivery acknowledgment
    FORWARD = 2,  // forwarded message to another client
    PREKEY = 3,         // response with a prekey
    PREKEYS_COUNT = 4,  // response with prekey count
    AUTH_SUCCESS = 5,   // successful authentication
    ONLINE_STATUS = 6,  // response with online status
    AUDIO_FRAME = 7,    // response with an audio frame
    SYNC_END = 8,  // response indicating sync completion
    SYNC_START = 9,  // response indicating sync start
    READED_SIGNAL = 10,    // response to the read signal
    RECEIVED_SIGNAL = 11,  // response to the received signal
    PROTOCOL_MISMATCH = 12,  // incompatible protocol version
    FCM_TOKEN_RECEIVED = 13,  // FCM token registration acknowledgment
    FCM_NOT_USED = 14,  // FCM is disabled on the server
    SESSION_REMOVED_SIGNAL = 15,  // response to the remove-chat signal
    FORWARD_LIVE = 16,  // live-forwarded message that requires ACK
    IDENTITY_REVOKED = 17,  // a peer published a key-revocation certificate
    ENROLL_ACK = 18,  // confirms a RequestEnrollMember was applied
    AUTHENTICATED = 19,  // handshake ok; sync about to start (still not "ready")
    CHUNK_BEGIN = 20,  // start of a chunked large frame: [type][transferId][total]
    CHUNK_DATA = 21,   // a chunk body: [type][transferId][bytes]
    CHUNK_END = 22     // end of a chunked frame -> reassemble + dispatch
  };

  enum class RequestType : uint8_t {  // client request types
    SEND_MESSAGE = 0,                 // send a message
    GET_PREKEY = 1,                   // request a prekey
    SEND_PREKEY = 2,                  // send a prekey
    COUNT_PREKEYS = 3,                // request the needed prekey count
    GET_ONLINE_STATUS = 4,  // request online status
    AUDIO_FRAME = 5,        // send an audio frame
    READED_SIGNAL = 6,  // send a read signal
    RECEIVED_SIGNAL = 7,  // send a received signal
    REGISTER_FCM_TOKEN = 8,  // register an FCM token
    SESSION_REMOVE_SIGNAL = 9,  // send a remove-chat signal
    MESSAGE_ACK = 10,  // acknowledge receipt of a live-forwarded message
    REVOKE_IDENTITY = 11,  // publish a signed "my key is compromised" certificate
    ENROLL_MEMBER = 12  // admit a contact's pubkey to this server's whitelist
  };

  // Non-sensitive routing hint sent as the first byte of a SEND_MESSAGE body so
  // the server knows which FCM push to emit for an offline recipient, without
  // ever inspecting the E2E-encrypted payload. Must match protocol.PushKind on
  // the server.
  enum class PushKind : uint8_t {
    Default = 0,     // regular message -> "new message" push when offline
    Call = 1,        // call_request -> data-only call wake push when offline
    Silent = 2,      // call accept/discard -> stored, never pushed
    CallCancel = 3,  // caller cancelled -> data-only stop-ringing push when offline
  };

  constexpr static uint8_t PREKEYS_MAX_SIZE = 100;

  explicit ConnectionManager(QObject* parent, ConnectionStorage* storage,
                             AccountManager* accountManager,
                             ContactsModel* contactsModel,
                             SettingsManager* settingsManager,
                             MessageModel* messageModel);
  ~ConnectionManager();

  Q_INVOKABLE void isOnline(const QString& serverId,
                            const QString& contactPubKey,
                            QJSValue callback = QJSValue());
  Q_INVOKABLE bool requestPreKey(const QString& serverId,
                                 const QString& clientPubKey,
                                 QJSValue callback = QJSValue());
  // inviteHex is the contact's invite secret from their QR/contact string
  // (hex); it seeds the initial Double Ratchet root key, so the session only
  // converges if it matches the invite the receiver currently holds.
  // expectedPubKey is the identity the caller actually intended to reach
  // (from the same QR/contact string, integrity-checked there) — it MUST be
  // compared against ed_pub before trusting the bundle. verify_prekey() only
  // proves the bundle is internally self-consistent (signed by whoever's key
  // is embedded in it); it says nothing about whose identity that is. Without
  // this check, a malicious/compromised server could hand back a bundle for
  // its own attacker-held identity instead of the intended contact's, and the
  // client would establish E2E with the attacker without any indication.
  Q_INVOKABLE bool startSessionWithPreKey(
      const QString& serverId, const QString& prekeyId, const QString& dh_pub,
      const QString& pq_pub, const QString& ed_pub, const QString& signature,
      const QString& expectedPubKey, const QString& inviteHex,
      QJSValue callback = QJSValue());
  Q_INVOKABLE void e2eCallRequest(const QString& serverId,
                                  const QString& contactPubKey, quint64 callId);

  // Connection management
  Q_INVOKABLE void addServer(const QString& serverUrl,
                             const QString& serverId = QString());
  Q_INVOKABLE void removeAllServers();

  // Panic action ("I've been hacked"): sign a key-revocation certificate with
  // our identity key and publish it. The tombstone is submitted to every
  // connected server (which then blocks new sessions against us, drops our
  // prekeys, and lazily warns anyone who messages us), and a signed in-band
  // notice is sent to each of our own contacts so they distrust the key at once.
  // Terminal and irreversible.
  Q_INVOKABLE void revokeIdentity();

  // Message sending
  Q_INVOKABLE bool sendMessage(const QString& serverId, const QString& message);
  Q_INVOKABLE bool sendBinaryMessage(const QString& serverId,
                                     const QByteArray& data);
  Q_INVOKABLE uint32_t request(const QString& serverId, RequestType requestType,
                               const QByteArray& data = QByteArray());
  Q_INVOKABLE void e2eSendSessionRequest(
      const QString& serverId, const QString& toPubKey,
      const pqdh::KeyPairs& ephemeralKeys, const std::vector<uint8_t>& prekeyId,
      const std::vector<uint8_t>& encapsulatedCiphertext);
  Q_INVOKABLE uint32_t e2eSendMessage(
      const QString& serverId, const QString& toPubKey,
      const QByteArray& plaintext,
      dr::EncryptedMessageFormat format = dr::EncryptedMessageFormat::BINARY,
      QJSValue callback = QJSValue());
  Q_INVOKABLE uint32_t e2eSendJSON(const QString& serverId,
                                   const QString& toPubKey,
                                   const QString& jsonStr,
                                   QJSValue callback = QJSValue());
  Q_INVOKABLE uint32_t e2eDeleteMessage(const QString& serverId,
                                        const QString& toPubKey,
                                        quint64 message_id,
                                        QJSValue callback = QJSValue());

  Q_INVOKABLE void sendReadedSignal(const QString& serverId,
                                    const QString& toPubKey);
  Q_INVOKABLE void sendReadSignal(const QString& serverId,
                                  const QString& toPubKey);
  Q_INVOKABLE uint32_t registerFcmToken(const QString& serverId,
                                        const QString& token);
  Q_INVOKABLE void translateAccountInfo();
  // Send our own accountInfo to a single peer (e.g. right after adding them).
  // It is a profile sync, not a message, so it goes out Silent — an offline
  // peer stores it without a spurious "new message" push.
  Q_INVOKABLE void sendOwnAccountInfo(const QString& serverId,
                                      const QString& toPubKey) {
    e2eSendJsonWithPush(serverId, toPubKey, buildOwnAccountInfoJson(false),
                        PushKind::Silent);
  }

  // calls
  Q_INVOKABLE void e2eCallDiscard(const QString& serverId,
                                  const QString& toPubKey,
                                  const uint64_t callId,
                                  const QString& reason = QString());
  Q_INVOKABLE void e2eCallCancel(const QString& serverId,
                                 const QString& toPubKey,
                                 const uint64_t callId);
  Q_INVOKABLE void e2eCallAccept(const QString& serverId,
                                 const QString& toPubKey,
                                 const std::vector<uint8_t>& dh_pub,
                                 const std::vector<uint8_t>& id,
                                 uint64_t callId);

  // Status checks
  Q_INVOKABLE bool isConnected(const QString& serverId) const;
  Q_INVOKABLE bool isAuthenticated(const QString& serverId) const;
  // Coarse connection lifecycle for the UI/troubleshooting:
  // 0 not connected · 2 authenticating · 3 synchronizing · 4 ready.
  Q_INVOKABLE int connectionPhase(const QString& serverId) const;
  Q_INVOKABLE QString getAccessToken(const QString& serverId) const;
  Q_INVOKABLE QString getHttpsBaseUrl(const QString& serverId) const;

  Q_INVOKABLE ConnectionStatus getStatus(const QString& serverId) const;
  Q_INVOKABLE QStringList getConnectedServers() const;
  Q_INVOKABLE QStringList getAllServers() const;

  // Settings
  Q_INVOKABLE void setReconnectInterval(int milliseconds);
  Q_INVOKABLE void setMaxReconnectAttempts(int attempts);
  Q_INVOKABLE void setConnectionTimeout(int milliseconds);
  Q_INVOKABLE void setPingInterval(int milliseconds);
  Q_INVOKABLE void setDeadConnectionTimeout(int milliseconds);
  Q_INVOKABLE bool isPrekeyGenerationActive(const QString& serverId) const;
  Q_INVOKABLE QString prekeyGenerationStatusText(const QString& serverId) const;

  Q_INVOKABLE QString getCallKeysFingerprint();
  void setCallSessionState(const CoreCrypto::SessionKeys& sessionKeys,
                           const QString& serverId,
                           const QString& contactPubKey);
  // Set by CallManager whenever a call UI is up (outgoing/ringing/active) so we
  // never tear the connection down on backgrounding mid-call.
  void setCallActive(bool active);
  CoreCrypto::SessionKeys callSessionKeys() const;
  QString currentCallServer() const;
  QString currentCallContactPubKey() const;

  void sendAudioFrame(const QString& serverId, const QString& toPubKey,
                      const QByteArray& encryptedFrame);

 signals:
  void messageReceived(const QString& serverId, const QString& message);
  void binaryMessageReceived(const QString& serverId, const QByteArray& data);
  void connectionStatusChanged(const QString& serverId,
                               ConnectionStatus status);
  void errorOccurred(const QString& serverId, const QString& errorMessage);
  void protocolVersionMismatch(const QString& serverId, const QString& message);
  void serverAdded(const QString& serverId);
  void serverRemoved(const QString& serverId);
  // Handshake confirmed by the server (ResponseAuthenticated), fired before
  // sync. authChanged(true) still fires later, after sync, when fully ready.
  void authenticated(const QString& serverId);
  void authChanged(const QString& serverId, bool success);
  void pongReceived(const QString& serverId, uint64_t elapsedTime);
  void textMessageReceived(const QString& serverId, const QString& fromPubKey,
                           const QString& message, const quint64 mid);
  void requestStateChanged(uint32_t requestId, const QString& status);
  void receivedSignalReceived(const QString& serverId,
                              const QString& fromPubKey, quint64 message_id);
  void callDiscardReceived(const QString& serverId,
                           const QString& contactPubKey, const quint64 callId,
                           const QString& reason);
  void callCancelReceived(const QString& serverId,
                          const QString& contactPubKey, const quint64 callId);
  void offlineSyncStarted(const QString& serverId);
  void offlineSyncEnded(const QString& serverId);
  void callRequestReceived(const QString& contactFirstName,
                           const QString& contactAvatar,
                           const QString& contactServer,
                           const QString& contactPubKey, const QString& dh_pub,
                           const QString& id, uint64_t callId);
  void callAcceptReceived();
  void audioFrameReceived(const QByteArray& frame);
  void contactAvatarChanged(const QString& serverId,
                            const QString& contactPubKey);
  void prekeyGenerationStateChanged(const QString& serverId, bool active,
                                    int generatedCount, int targetCount);
  void fcmTokenRegistered(const QString& serverId, uint32_t requestId);
  void fcmNotUsed(const QString& serverId, uint32_t requestId);

 public slots:
  void addServers();
  void onMessageDelete(const quint64 message_id, const QString& serverId,
                       const QString& contactPubKey);
  void onContactRemove(const QString& serverId, const QString& contactPubKey);

 private slots:
  void onTransportBinaryMessageReceived(const QString& serverId,
                                        const QByteArray& data);
  void onTransportDisconnected(const QString& serverId);
  void onTransportError(const QString& serverId, const QString& errorMessage);
  void onTransportSslError(const QString& serverId,
                           const QString& errorMessage);
  void onOnlineRequest(const QString& serverAddress,
                       const QString& contactPubKey);
  void onClearHistory(const QString& serverId, const QString& contactPubKey);
  void onSetTimer(const QString& serverId, const QString& contactPubKey,
                  int seconds);
  void onPeriodicPrekeySyncTimeout();
  // Android only: drop the connection when the app is backgrounded (so the
  // server marks us offline and routes incoming calls/messages through FCM,
  // which can surface them) and reconnect + resync when it returns to the
  // foreground. A backgrounded app cannot raise the call UI and the server
  // would otherwise keep us "online" for ~75s, missing the call.
  void onApplicationStateChanged(Qt::ApplicationState state);

 private:
  struct PendingRetryRequest {
    QString serverId;
    QString contactPubKey;
    quint64 messageId = 0;
    QString text;
    quint64 timestamp = 0;
  };

  // Shared send path that prepends the push-kind hint byte; the public
  // e2eSendMessage/e2eSendJSON forward here with PushKind::Default.
  uint32_t e2eSendMessageWithPush(const QString& serverId,
                                  const QString& toPubKey,
                                  const QByteArray& plaintext,
                                  dr::EncryptedMessageFormat format,
                                  PushKind pushKind, QJSValue callback);
  uint32_t e2eSendJsonWithPush(const QString& serverId, const QString& toPubKey,
                               const QString& jsonStr, PushKind pushKind);

  void requestPrekeysSync(const QString& serverId);
  // Tear down / restore all connections for the background/foreground lifecycle
  // (Android). goOffline aborts sockets so the server marks us offline (→ FCM);
  // goOnline reconnects and replays the offline queue.
  void goOffline();
  void goOnline();
  void sendSessionRemoveSignal(const QString& serverId,
                               const QString& toPubKey);
  void removeServer(const QString& serverId);
  QString generateServerId(const QString& serverUrl);
  ConnectionServerRuntimeState& stateForServer(const QString& serverId);
  const ConnectionServerRuntimeState* stateForServer(
      const QString& serverId) const;
  void invokeCallback(uint32_t requestId, const QJSValueList& args,
                      const QString& context, const QString& serverId,
                      const QString& state = QString());
  void sendReceivedSignalForMessage(const QString& serverId,
                                    const QString& fromPubKey,
                                    quint64 messageId);
  // Admits a contact's pubkey to this server's whitelist. Sent by an existing
  // member when a peer that scanned their QR successfully establishes a session
  // (server ignores it unless we are ourselves whitelisted). Server-side this is
  // RequestEnrollMember; see the server membership gate. Tracks the request so
  // handleEnrollAcked() can mark it confirmed; if the connection drops before
  // an ack arrives (or before this could even be sent), the contact stays a
  // retry candidate for retryPendingMemberEnrollments().
  void enrollMember(const QString& serverId, const QString& contactPubKey);
  // Resends enrollMember() for every materialized contact on this server the
  // server has not yet acknowledged as enrolled. Called on every successful
  // (re)connect so a one-shot enroll lost to a connection drop is not lost
  // forever - the server-side add is idempotent, so repeating it is harmless.
  void retryPendingMemberEnrollments(const QString& serverId);
  void handleEnrollAcked(const QString& serverId, uint32_t requestId);
  QString buildOwnAccountInfoJson(bool isResponse) const;
  void retryPendingTextMessages(const QString& serverId);
  void handlePendingRetryRequestState(uint32_t requestId, const QString& state);
  QString pendingRetryMessageKey(const QString& serverId,
                                 const QString& contactPubKey,
                                 quint64 messageId) const;
  void clearPendingRetryRequests(const QString& serverId);
  bool isHomeServer(const QString& serverId) const;
  bool serverHasContacts(const QString& serverId) const;
  void cleanupServerIfUnused(const QString& serverId);
  int prekeyTargetCount() const;
  int prekeySyncIntervalMinutes() const;
  void updatePeriodicPrekeySyncTimer();

  // True while connections are torn down because the app is backgrounded, so we
  // know to reconnect on return and avoid redundant offline/online cycles.
  bool m_suspendedOffline = false;
  // True while a call UI is on screen; suppresses the background disconnect so a
  // call is never dropped by pressing Home.
  bool m_callActive = false;
  // Tracks whether the app is currently backgrounded, so a call that ends while
  // backgrounded can then take us offline.
  bool m_appBackgrounded = false;

  mutable QRecursiveMutex
      tdMutex_;  // ! protects against deadlocks in DoubleRatchet during nested calls

  uint32_t lastRequestId_;
  QHash<uint32_t, QJSValue> callbacks_;

  CoreCrypto::SessionKeys m_callKeys;
  QString m_currentContactServer;
  QString m_currentContactPubKey;
  QThread* m_transportThread;
  ConnectionTransport* m_transport;
  QHash<QString, ConnectionServerRuntimeState> m_serverState;
  QHash<uint32_t, PendingRetryRequest> m_pendingRetryRequests;
  QSet<QString> m_pendingRetryMessageKeys;
  // In-flight ENROLL_MEMBER requests awaiting a ResponseEnrollAck, keyed by
  // requestId, so the ack can be matched back to (serverId, contactPubKey).
  QHash<uint32_t, QPair<QString, QString>> m_pendingEnrollRequests;
  QHash<QString, QString> m_prekeyGenerationStatusTexts;
  QSet<QString> m_prekeyGenerationActiveServers;
  ConnectionStorage* m_storage;
  AccountManager* m_accountManager;
  ContactsModel* m_contactsModel;
  SettingsManager* m_settings;
  MessageModel* m_messageModel;
  std::unique_ptr<ConnectionProtocolHandler> m_protocolHandler;
  std::unique_ptr<class ConnectionPayloadProcessor> m_payloadProcessor;
  QTimer m_periodicPrekeySyncTimer;
};

#endif  // CONNECTIONMANAGER_H