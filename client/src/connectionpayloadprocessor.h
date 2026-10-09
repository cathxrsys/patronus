#ifndef CONNECTIONPAYLOADPROCESSOR_H
#define CONNECTIONPAYLOADPROCESSOR_H

#include <corecrypto.h>

#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

class AccountManager;
class ConnectionStorage;
class ContactsModel;
class MessageModel;
class NameStyleValidator;

class ConnectionPayloadProcessor {
 public:
  struct Callbacks {
    std::function<void(const QString&, const QString&, quint64)>
        sendReceivedSignalForMessage;
    std::function<uint32_t(const QString&, const QString&, const QString&)>
        sendJson;
    std::function<QString(bool)> buildOwnAccountInfoJson;
    std::function<void(const CoreCrypto::SessionKeys&, const QString&,
                       const QString&)>
        setCallSessionState;
    std::function<void(const QString&, const QString&, const QString&, quint64)>
        textMessageReceived;
    std::function<void(const QString&, const QString&)> contactAvatarChanged;
    std::function<void(const QString&, const QString&, quint64, const QString&)>
        callDiscardReceived;
    std::function<void(const QString&, const QString&, quint64)>
        callCancelReceived;
    std::function<void()> callAcceptReceived;
    std::function<void(const QString&, const QString&, const QString&,
                       const QString&, const QString&, const QString&,
                       uint64_t)>
        callRequestReceived;
    // Returns our current invite secret; it seeds the initial Double Ratchet
    // root key for incoming session_requests, so only initiators who hold the
    // invite can produce messages we can decrypt.
    std::function<QByteArray()> getOwnInviteSecret;
    // Admits a peer (serverId, pubKey) to the server whitelist. Called once when
    // a peer that scanned our QR proves the invite (first successful decrypt);
    // the server honours it only if we are a member.
    std::function<void(const QString&, const QString&)> enrollMember;
  };

  ConnectionPayloadProcessor(ConnectionStorage* storage,
                             AccountManager* accountManager,
                             ContactsModel* contactsModel,
                             MessageModel* messageModel, Callbacks callbacks);
  // Out-of-line so the unique_ptr to the forward-declared NameStyleValidator can
  // be destroyed where the type is complete.
  ~ConnectionPayloadProcessor();

  void handleForwardFrame(const QString& serverId, const QByteArray& data);
  void handleJsonMessage(const QString& serverId, const QString& fromPubKey,
                         const nlohmann::json& msg);
  void handleBinaryMessage(const QString& serverId, const QString& fromPubKey,
                           const QByteArray& data);

 private:
  quint64 parseMessageId(const nlohmann::json& msg) const;
  quint64 parseMessageTimestamp(const nlohmann::json& msg) const;
  void handleAccountInfoMessage(const QString& serverId,
                                const QString& fromPubKey,
                                const nlohmann::json& msg);
  void handleDeleteMessageJson(const QString& serverId,
                               const QString& fromPubKey,
                               const nlohmann::json& msg);
  void handleTextMessageJson(const QString& serverId, const QString& fromPubKey,
                             const nlohmann::json& msg);
  void handleMediaMessageJson(const QString& serverId,
                              const QString& fromPubKey,
                              const nlohmann::json& msg, const QString& type);
  void handleAlbumMessageJson(const QString& serverId,
                              const QString& fromPubKey,
                              const nlohmann::json& msg);
  void handleCallAcceptMessage(const QString& serverId,
                               const QString& fromPubKey,
                               const nlohmann::json& msg);
  void handleCallRequestMessage(const QString& serverId,
                                const QString& fromPubKey,
                                const nlohmann::json& msg);

  ConnectionStorage* m_storage;
  AccountManager* m_accountManager;
  ContactsModel* m_contactsModel;
  MessageModel* m_messageModel;
  Callbacks m_callbacks;
  std::unique_ptr<NameStyleValidator> m_nameStyleValidator;
};

#endif