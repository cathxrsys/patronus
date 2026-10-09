#ifndef MessageMODEL_H
#define MessageMODEL_H

#include <QAbstractListModel>
#include <QDateTime>
#include <QList>
#include <QRandomGenerator>

#include "contactsmodel.h"
#include "filemessageutils.h"
#include "mainsignals.h"

class DatabaseManager;

// Denormalized snapshot of the message a reply quotes. Carried through the
// send/receive/DB paths so a reply can render its one-line quote without a
// lookup and survive the original being deleted. replyTo == 0 means "not a
// reply". kind: "" (text), "image", "video", "audio", "file", "album".
struct ReplyInfo {
  quint64 replyTo = 0;
  QString preview;
  QString kind;
  bool isOwn = false;  // authored by this side (relative to the local DB)
};

struct Message {
  quint64 id = 0;
  quint64 mid = 0;
  QString text;
  QString time;
  bool isOwn = false;
  bool isMsg = false;
  bool isDate = false;
  bool isCall = false;
  quint64 callDurationSec = 0;
  bool isFile = false;
  bool isAudio = false;
  bool isInfo = false;
  bool isSended = false;
  bool isReaded = false;
  bool isReceived = false;
  quint64 t = 0;  // timestamp
  FileMessageData fileData;
  bool fileTransferPending = false;
  bool fileTransferCancelable = false;
  qreal fileTransferProgress = 0.0;
  QString fileTransferStatusText;
  quint64 replyTo = 0;
  QString replyPreview;
  QString replyKind;
  bool replyIsOwn = false;
  bool isAlbum = false;
  AlbumMessageData albumData;
  // Per-item download state (runtime only, sized to albumData.items).
  QList<bool> albumItemDownloading;
  QList<qreal> albumItemProgress;
};

Q_DECLARE_METATYPE(Message)
Q_DECLARE_METATYPE(QList<Message>)

class MessageModel : public QAbstractListModel {
  Q_OBJECT

 public:
  enum ChatRoles {
    IdRole = Qt::UserRole + 1,
    MIdRole,
    TextRole,
    TimeRole,
    IsOwnRole,
    IsMsgRole,
    IsDateRole,
    IsCallRole,
    CallDurationRole,
    IsSendedRole,
    IsReadedRole,
    IsReceivedRole,
    IsInfoRole,
    IsFileRole,
    IsAudioRole,
    IsImageRole,
    IsVideoRole,
    FileNameRole,
    FileSizeRole,
    FileMediaTypeRole,
    FileMimeTypeRole,
    AudioDurationRole,
    AudioWaveformRole,
    FileLocalPathRole,
    FileDownloadedRole,
    FileTransferPendingRole,
    FileTransferCancelableRole,
    FileTransferProgressRole,
    FileTransferStatusTextRole,
    ReplyToRole,
    ReplyPreviewRole,
    ReplyKindRole,
    ReplyIsOwnRole,
    IsAlbumRole,
    AlbumItemsRole,
    AlbumProgressRole
  };

  explicit MessageModel(QObject* parent = nullptr,
                        DatabaseManager* dbManager = nullptr,
                        ContactsModel* contactsModel = nullptr);

  enum MESSAGE_TYPE : uint8_t {
    TEXT = 0,
    CALL = 1,
    INFO = 2,
    FILE = 3,
    AUDIO = 4,
    ALBUM = 5
  };

  // Core QAbstractListModel methods
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index,
                int role = Qt::DisplayRole) const override;
  QHash<int, QByteArray> roleNames() const override;

  // Data access methods
  Q_INVOKABLE void addMessage(const quint64 id, const QString& text,
                              const QString& time, bool isOwn, bool isMsg,
                              bool isDate, bool isCall, bool isSended,
                              bool isReaded, bool isReceived,
                              MESSAGE_TYPE type = TEXT,
                              const QByteArray& content = QByteArray(),
                              quint64 timestamp = 0,
                              const ReplyInfo& reply = {});
  Q_INVOKABLE void addInfoMessage(const QString& serverId,
                                  const QString& pubKeyFingerprint,
                                  const QString& text);

  Q_INVOKABLE void clear();

  Q_INVOKABLE void loadFromDatabase(const QString& serverId,
                                    const QString& contactPubKey);
  Q_INVOKABLE void sendMessage(const quint64 id, const QString& serverId,
                               const QString& contactPubKey,
                               const QString& text, quint64 timestamp = 0,
                               quint64 replyTo = 0,
                               const QString& replyPreview = QString(),
                               const QString& replyKind = QString(),
                               bool replyIsOwn = false);
  Q_INVOKABLE void receiveMessage(const quint64 id, const QString& serverId,
                                  const QString& contactPubKey,
                                  const QString& text, quint64 timestamp = 0,
                                  const ReplyInfo& reply = {});
  Q_INVOKABLE void sendFileMessage(const quint64 id, const QString& serverId,
                                   const QString& contactPubKey,
                                   const QString& text,
                                   const QByteArray& content,
                                   quint64 timestamp = 0,
                                   const ReplyInfo& reply = {});
  Q_INVOKABLE void receiveFileMessage(const quint64 id, const QString& serverId,
                                      const QString& contactPubKey,
                                      const QString& text,
                                      const QByteArray& content,
                                      quint64 timestamp = 0,
                                      const ReplyInfo& reply = {});
  Q_INVOKABLE void addPendingFileMessage(
      const quint64 id, const QString& serverId, const QString& contactPubKey,
      const QString& text, const QByteArray& content, quint64 timestamp = 0,
      const ReplyInfo& reply = {});
  Q_INVOKABLE void sendAudioMessage(const quint64 id, const QString& serverId,
                                    const QString& contactPubKey,
                                    const QString& text,
                                    const QByteArray& content,
                                    quint64 timestamp = 0,
                                    const ReplyInfo& reply = {});
  Q_INVOKABLE void receiveAudioMessage(
      const quint64 id, const QString& serverId, const QString& contactPubKey,
      const QString& text, const QByteArray& content, quint64 timestamp = 0,
      const ReplyInfo& reply = {});
  Q_INVOKABLE void addPendingAudioMessage(
      const quint64 id, const QString& serverId, const QString& contactPubKey,
      const QString& text, const QByteArray& content, quint64 timestamp = 0,
      const ReplyInfo& reply = {});
  Q_INVOKABLE void receiveAlbumMessage(
      const quint64 id, const QString& serverId, const QString& contactPubKey,
      const QString& caption, const QByteArray& content, quint64 timestamp = 0,
      const ReplyInfo& reply = {});
  Q_INVOKABLE void addPendingAlbumMessage(
      const quint64 id, const QString& serverId, const QString& contactPubKey,
      const QString& caption, const QByteArray& content, quint64 timestamp = 0,
      const ReplyInfo& reply = {});
  Q_INVOKABLE void completePendingAlbumMessage(const QString& serverId,
                                               const QString& contactPubKey,
                                               const quint64 id,
                                               const QByteArray& content);
  Q_INVOKABLE void updateAlbumItemLocalPath(const QString& serverId,
                                            const QString& contactPubKey,
                                            const quint64 id, int itemIndex,
                                            const QString& localPath);
  Q_INVOKABLE void setAlbumItemDownloading(const QString& serverId,
                                           const QString& contactPubKey,
                                           const quint64 id, int itemIndex,
                                           bool downloading, qreal progress);
  Q_INVOKABLE void updateMessageTimestamp(const QString& serverId,
                                          const QString& contactPubKey,
                                          const quint64 id, quint64 timestamp);
  Q_INVOKABLE void updateMessageSendedStatus(const QString& serverId,
                                             const QString& contactPubKey,
                                             const quint64 id);
  Q_INVOKABLE void updateMessageSentStatus(const QString& serverId,
                                           const QString& contactPubKey,
                                           const quint64 id);
  Q_INVOKABLE void updateMessageDeliveredStatus(const QString& serverId,
                                                const QString& contactPubKey,
                                                const quint64 id);
  Q_INVOKABLE void updateCallDuration(const QString& serverId,
                                      const QString& contactPubKey,
                                      const quint64 id, quint64 durationSec);
  void addCallStatusMessageIfCurrentChat(const QString& serverId,
                                         const QString& contactPubKey,
                                         const quint64 id, const QString& text,
                                         bool isOwn);
  Q_INVOKABLE void updateFileLocalPath(const QString& serverId,
                                       const QString& contactPubKey,
                                       const quint64 id,
                                       const QString& localPath);
  Q_INVOKABLE void updateFileTransferProgress(const quint64 id, qreal progress);
  Q_INVOKABLE void setFileTransferState(const QString& serverId,
                                        const QString& contactPubKey,
                                        const quint64 id, bool pending,
                                        bool cancelable, qreal progress,
                                        const QString& statusText);
  Q_INVOKABLE void completePendingFileMessage(const QString& serverId,
                                              const QString& contactPubKey,
                                              const quint64 id,
                                              const QByteArray& content);
  Q_INVOKABLE void removeTransientMessage(const QString& serverId,
                                          const QString& contactPubKey,
                                          const quint64 id);
  Q_INVOKABLE void markMessagesAsReaded(const QString& serverId,
                                        const QString& contactPubKey);
  Q_INVOKABLE void markMessagesAsRead(const QString& serverId,
                                      const QString& contactPubKey);
  Q_INVOKABLE void markOwnMessagesAsReaded(const QString& serverId,
                                           const QString& contactPubKey);
  Q_INVOKABLE void markOwnMessagesAsRead(const QString& serverId,
                                         const QString& contactPubKey);
  Q_INVOKABLE bool hasUnreadMessages(const QString& serverId,
                                     const QString& contactPubKey);
  Q_INVOKABLE bool isCurrentChat(const QString& serverId,
                                 const QString& contactPubKey) const;
  Q_INVOKABLE void closeCurrentChat();
  Q_INVOKABLE void deleteMessage(const QString& serverId,
                                 const QString& contactPubKey,
                                 const quint64 id);

  Q_INVOKABLE quint64 uniqId();

  // Row index of the message with this id, or -1. Used by reply quotes to jump
  // to the quoted message.
  Q_INVOKABLE int indexOfMessageId(quint64 id) const;

  bool hasMessage(quint64 id) const;

  void setMessages(const QList<Message>& messages);

 signals:
  void messageStatusUpdated(int index);
  void chatToBottom();
  void messagesLoaded(const QString& serverId, const QString& contactPubKey);

 public slots:
  void onMessageDelete(const quint64 message_id, const QString& serverId,
                       const QString& contactPubKey);
  void onMessagesLoaded(const QString& serverId, const QString& contactPubKey,
                        const QList<Message>& messages);

 private:
  QList<Message> m_messages;
  QString m_currentContactServer = "";
  QString m_currentContactPubKey = "";

  int findMessageIndex(quint64 id, bool requireFile = false) const;
  int findLastNonDateMessageIndex() const;
  DatabaseManager* m_db;
  ContactsModel* m_contactsModel;
};

#endif  // MessageMODEL_H