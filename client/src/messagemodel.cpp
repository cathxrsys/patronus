#include "messagemodel.h"

#include "apppaths.h"
#include "contactsmodel.h"
#include "databasemanager.h"
#include "localeutils.h"
#include "voicecliputils.h"

namespace {

QString formatMessageTime(quint64 timestamp) {
  return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp))
      .toString("HH:mm");
}

void ensureAudioWaveform(FileMessageData* fileData) {
  if (fileData == nullptr || !filemessage::isAudioMessage(*fileData) ||
      !fileData->waveform.isEmpty() || fileData->localPath.isEmpty()) {
    return;
  }

  fileData->waveform = voiceclip::buildWaveformFromFile(fileData->localPath);
}

// Builds the QML-facing list of album items (structural data only). This must
// stay stable across download-progress ticks, otherwise the QML Repeaters bound
// to it would rebuild every grid/list delegate on each tick and freeze the UI —
// transient download state lives in buildAlbumProgress() instead.
QVariantList buildAlbumItems(const Message& message) {
  QVariantList result;
  const auto& items = message.albumData.items;
  result.reserve(items.size());
  for (int i = 0; i < items.size(); ++i) {
    const FileMessageData& item = items[i];
    QVariantMap map;
    map["index"] = i;
    map["fileName"] = item.fileName;
    map["fileSize"] = QVariant::fromValue(item.fileSize);
    map["mediaType"] = item.mediaType;
    map["mimeType"] = item.mimeType;
    map["kind"] = filemessage::mediaKind(item);
    map["isImage"] = filemessage::isImageMessage(item);
    map["isVideo"] = filemessage::isVideoMessage(item);
    map["isAudio"] = filemessage::isAudioMessage(item);
    map["localPath"] = item.localPath;
    map["downloaded"] = filemessage::hasLocalFile(item);
    map["durationMs"] = QVariant::fromValue(item.durationMs);
    result.append(map);
  }
  return result;
}

// Per-item transient download state, emitted separately (AlbumProgressRole) so
// frequent progress updates don't invalidate the structural item list.
QVariantList buildAlbumProgress(const Message& message) {
  QVariantList result;
  const int count = message.albumData.items.size();
  result.reserve(count);
  for (int i = 0; i < count; ++i) {
    QVariantMap map;
    map["downloading"] =
        (i < message.albumItemDownloading.size()) && message.albumItemDownloading[i];
    map["progress"] =
        (i < message.albumItemProgress.size()) ? message.albumItemProgress[i] : 0.0;
    result.append(map);
  }
  return result;
}

}  // namespace

MessageModel::MessageModel(QObject* parent, DatabaseManager* dbManager,
                           ContactsModel* contactsModel)
    : QAbstractListModel(parent),
      m_db(dbManager),
      m_contactsModel(contactsModel) {
  if (m_db != nullptr) {
    connect(m_db, &DatabaseManager::messagesLoaded, this,
            &MessageModel::onMessagesLoaded);
  }

  connect(&MainSignals::instance(), &MainSignals::allMessagesReaded, this,
          &MessageModel::markMessagesAsReaded);
  connect(&MainSignals::instance(), &MainSignals::allOwnMessagesReaded, this,
          &MessageModel::markOwnMessagesAsReaded);

  connect(&MainSignals::instance(), &MainSignals::messageDelete, this,
          &MessageModel::onMessageDelete);
  connect(&MainSignals::instance(), &MainSignals::localMessageDelete, this,
          &MessageModel::onMessageDelete);
  connect(&MainSignals::instance(), &MainSignals::messageDeleteReceived, this,
          &MessageModel::onMessageDelete);

  connect(
      &MainSignals::instance(), &MainSignals::e2eTextMessageReceived, this,
      [this](const QString& serverId, const QString& fromPubKey,
             const QString& messageText, quint64 messageId, quint64 timestamp,
             quint64 replyTo, const QString& replyPreview,
             const QString& replyKind, bool replyIsOwn) {
        receiveMessage(messageId, serverId, fromPubKey, messageText, timestamp,
                       ReplyInfo{replyTo, replyPreview, replyKind, replyIsOwn});
      });

  auto clearHistoryHandler = [this](const QString& serverId,
                                    const QString& contactPubKey) {
    if (serverId == m_currentContactServer &&
        contactPubKey == m_currentContactPubKey) {
      clear();
    }
    m_contactsModel->setLastMessage(serverId, contactPubKey, "", "");
    m_contactsModel->setUnreadedCount(serverId, contactPubKey, 0);
  };

  auto onSetTimer = [this](const QString& serverId,
                           const QString& contactPubKey, int timerValue) {
    QString text;
    if (serverId == m_currentContactServer &&
        contactPubKey == m_currentContactPubKey) {
      if (timerValue == 0) {
        text = tr("Timer is disabled");
        addInfoMessage(serverId, contactPubKey, text);
      } else {
        text = tr("Timer is set for %1").arg(m_db->getTimerValueS(timerValue));
        addInfoMessage(serverId, contactPubKey, text);
      }
    }
  };

  connect(&MainSignals::instance(), &MainSignals::setTimer, this, onSetTimer);
  connect(&MainSignals::instance(), &MainSignals::setTimerReceived, this,
          onSetTimer);

  connect(&MainSignals::instance(), &MainSignals::clearHistory, this,
          clearHistoryHandler);
  connect(&MainSignals::instance(), &MainSignals::clearHistoryReceived, this,
          clearHistoryHandler);
}

bool MessageModel::hasMessage(quint64 id) const {
  return findMessageIndex(id) >= 0;
}

int MessageModel::indexOfMessageId(quint64 id) const {
  return findMessageIndex(id);
}

int MessageModel::findMessageIndex(quint64 id, bool requireFile) const {
  for (int i = 0; i < m_messages.size(); ++i) {
    if (m_messages[i].id == id && (!requireFile || m_messages[i].isFile)) {
      return i;
    }
  }

  return -1;
}

int MessageModel::findLastNonDateMessageIndex() const {
  for (int i = m_messages.size() - 1; i >= 0; --i) {
    if (!m_messages[i].isDate) {
      return i;
    }
  }

  return -1;
}

int MessageModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;

  return m_messages.count();
}

QVariant MessageModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= m_messages.count()) return QVariant();

  const Message& message = m_messages.at(index.row());

  switch (role) {
    case IdRole:
      return message.id;
    case MIdRole:
      return message.id;
    case TextRole:
      return message.text;
    case TimeRole:
      return message.time;
    case IsOwnRole:
      return message.isOwn;
    case IsMsgRole:
      return message.isMsg;
    case IsDateRole:
      return message.isDate;
    case IsCallRole:
      return message.isCall;
    case CallDurationRole:
      return QVariant::fromValue(message.callDurationSec);
    case IsSendedRole:
      return message.isSended;
    case IsReadedRole:
      return message.isReaded;
    case IsReceivedRole:
      return message.isReceived;
    case IsInfoRole:
      return message.isInfo;
    case IsFileRole:
      return message.isFile;
    case IsAudioRole:
      return message.isAudio;
    case IsImageRole:
      return filemessage::isImageMessage(message.fileData);
    case IsVideoRole:
      return filemessage::isVideoMessage(message.fileData);
    case FileNameRole:
      return message.fileData.fileName;
    case FileSizeRole:
      return QVariant::fromValue(message.fileData.fileSize);
    case FileMediaTypeRole:
      return message.fileData.mediaType;
    case FileMimeTypeRole:
      return message.fileData.mimeType;
    case AudioDurationRole:
      return QVariant::fromValue(message.fileData.durationMs);
    case AudioWaveformRole: {
      QVariantList waveform;
      waveform.reserve(message.fileData.waveform.size());
      for (int value : message.fileData.waveform) {
        waveform.append(value);
      }
      return waveform;
    }
    case FileLocalPathRole:
      return message.fileData.localPath;
    case FileDownloadedRole:
      return filemessage::hasLocalFile(message.fileData);
    case FileTransferPendingRole:
      return message.fileTransferPending;
    case FileTransferCancelableRole:
      return message.fileTransferCancelable;
    case FileTransferProgressRole:
      return message.fileTransferProgress;
    case FileTransferStatusTextRole:
      return message.fileTransferStatusText;
    case ReplyToRole:
      return QVariant::fromValue(message.replyTo);
    case ReplyPreviewRole:
      return message.replyPreview;
    case ReplyKindRole:
      return message.replyKind;
    case ReplyIsOwnRole:
      return message.replyIsOwn;
    case IsAlbumRole:
      return message.isAlbum;
    case AlbumItemsRole:
      return buildAlbumItems(message);
    case AlbumProgressRole:
      return buildAlbumProgress(message);
    default:
      return QVariant();
  }
}

QHash<int, QByteArray> MessageModel::roleNames() const {
  QHash<int, QByteArray> roles;
  roles[IdRole] = "id";
  roles[MIdRole] = "mid";
  roles[TextRole] = "text";
  roles[TimeRole] = "time";
  roles[IsOwnRole] = "isOwn";
  roles[IsMsgRole] = "isMsg";
  roles[IsDateRole] = "isDate";
  roles[IsCallRole] = "isCall";
  roles[CallDurationRole] = "callDurationSec";
  roles[IsSendedRole] = "isSended";
  roles[IsReadedRole] = "isReaded";
  roles[IsReceivedRole] = "isReceived";
  roles[IsInfoRole] = "isInfo";
  roles[IsFileRole] = "isFile";
  roles[IsAudioRole] = "isAudio";
  roles[IsImageRole] = "isImage";
  roles[IsVideoRole] = "isVideo";
  roles[FileNameRole] = "fileName";
  roles[FileSizeRole] = "fileSize";
  roles[FileMediaTypeRole] = "fileMediaType";
  roles[FileMimeTypeRole] = "fileMimeType";
  roles[AudioDurationRole] = "audioDurationMs";
  roles[AudioWaveformRole] = "audioWaveform";
  roles[FileLocalPathRole] = "fileLocalPath";
  roles[FileDownloadedRole] = "fileDownloaded";
  roles[FileTransferPendingRole] = "fileTransferPending";
  roles[FileTransferCancelableRole] = "fileTransferCancelable";
  roles[FileTransferProgressRole] = "fileTransferProgress";
  roles[FileTransferStatusTextRole] = "fileTransferStatusText";
  roles[ReplyToRole] = "replyTo";
  roles[ReplyPreviewRole] = "replyPreview";
  roles[ReplyKindRole] = "replyKind";
  roles[ReplyIsOwnRole] = "replyIsOwn";
  roles[IsAlbumRole] = "isAlbum";
  roles[AlbumItemsRole] = "albumItems";
  roles[AlbumProgressRole] = "albumProgress";
  return roles;
}

void MessageModel::addMessage(const quint64 id, const QString& text,
                              const QString& time, bool isOwn, bool isMsg,
                              bool isDate, bool isCall, bool isSended,
                              bool isReaded, bool isReceived, MESSAGE_TYPE type,
                              const QByteArray& content, quint64 timestamp,
                              const ReplyInfo& reply) {
  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(QDateTime::currentSecsSinceEpoch());

  Message lastMessage = m_messages.isEmpty() ? Message{} : m_messages.last();

  QDate date1 = QDateTime::fromSecsSinceEpoch(lastMessage.t).date();
  QDate date2 = QDateTime::fromSecsSinceEpoch(effectiveTimestamp).date();

  if (!m_messages.isEmpty() && date1 != date2 && isDate == false) {
    QSettings settings(apppaths::settingsFilePath(), QSettings::IniFormat);
    const QLocale locale = localeutils::qLocaleForUiLanguage(
        settings.value("ui/language", "en").toString());

    addMessage(uniqId(), locale.toString(date2, "d MMMM"), "", false, false,
               true, false, true, true, true);
  }

  Message message;
  message.id = id;
  message.mid = id;
  message.text = text;
  message.time = time;
  message.isOwn = isOwn;
  message.isMsg = isMsg;
  message.isDate = isDate;
  message.isCall = isCall;
  message.isFile = (type == MESSAGE_TYPE::FILE || type == MESSAGE_TYPE::AUDIO);
  message.isAudio = (type == MESSAGE_TYPE::AUDIO);
  message.isAlbum = (type == MESSAGE_TYPE::ALBUM);
  message.isSended = isSended;
  message.isReaded = isReaded;
  message.isReceived = isReceived;
  message.t = effectiveTimestamp;
  if (message.isFile) {
    filemessage::deserialize(content, &message.fileData);
    ensureAudioWaveform(&message.fileData);
  }
  if (message.isAlbum) {
    filemessage::deserializeAlbum(content, &message.albumData);
    message.albumItemDownloading =
        QList<bool>(message.albumData.items.size(), false);
    message.albumItemProgress =
        QList<qreal>(message.albumData.items.size(), 0.0);
  }
  message.fileTransferPending = false;
  message.fileTransferCancelable = false;
  message.fileTransferProgress = 0.0;
  message.fileTransferStatusText.clear();
  message.replyTo = reply.replyTo;
  message.replyPreview = reply.preview;
  message.replyKind = reply.kind;
  message.replyIsOwn = reply.isOwn;

  beginInsertRows(QModelIndex(), m_messages.count(), m_messages.count());

  m_messages.append(message);

  endInsertRows();

  emit chatToBottom();
}

void MessageModel::addInfoMessage(const QString& serverId,
                                  const QString& pubKeyFingerprint,
                                  const QString& text) {
  if (serverId != m_currentContactServer ||
      pubKeyFingerprint != m_currentContactPubKey) {
    return;
  }

  Message lastMessage = m_messages.isEmpty() ? Message{} : m_messages.last();

  Message message;
  message.id = lastMessage.id + 1;
  message.mid = message.id;
  message.text = text;
  message.time = "";
  message.isOwn = false;
  message.isMsg = false;
  message.isDate = false;
  message.isCall = false;
  message.isSended = true;
  message.isReaded = true;
  message.isReceived = true;
  message.t = QDateTime::currentSecsSinceEpoch();
  message.isInfo = true;

  beginInsertRows(QModelIndex(), m_messages.count(), m_messages.count());

  m_messages.append(message);

  endInsertRows();

  emit chatToBottom();
}

void MessageModel::clear() {
  beginResetModel();
  m_messages.clear();
  endResetModel();
}

void MessageModel::loadFromDatabase(const QString& serverId,
                                    const QString& contactPubKey) {
  m_currentContactServer = serverId;
  m_currentContactPubKey = contactPubKey;

  beginResetModel();
  m_messages.clear();
  endResetModel();

  m_contactsModel->setUnreadedCount(serverId, contactPubKey, 0);

  if (m_db != nullptr) {
    m_db->requestMessagesLoad(serverId, contactPubKey);
  }
}

void MessageModel::onMessagesLoaded(const QString& serverId,
                                    const QString& contactPubKey,
                                    const QList<Message>& messages) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  setMessages(messages);
}

void MessageModel::setMessages(const QList<Message>& messages) {
  beginResetModel();
  m_messages = messages;
  endResetModel();

  emit chatToBottom();
  emit messagesLoaded(m_currentContactServer, m_currentContactPubKey);
}

void MessageModel::sendMessage(const quint64 id, const QString& serverId,
                               const QString& contactPubKey,
                               const QString& text, quint64 timestamp,
                               quint64 replyTo, const QString& replyPreview,
                               const QString& replyKind, bool replyIsOwn) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;
  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
  const QString timeString = formatMessageTime(effectiveTimestamp);

  addMessage(id, text, timeString, true, true, false, false, false, false,
             false, MESSAGE_TYPE::TEXT, QByteArray(), effectiveTimestamp,
             ReplyInfo{replyTo, replyPreview, replyKind, replyIsOwn});
}

void MessageModel::receiveMessage(const quint64 id, const QString& serverId,
                                  const QString& contactPubKey,
                                  const QString& text, quint64 timestamp,
                                  const ReplyInfo& reply) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;
  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
  const QString timeString = formatMessageTime(effectiveTimestamp);

  addMessage(id, text, timeString, false, true, false, false, false, false,
             false, MESSAGE_TYPE::TEXT, QByteArray(), effectiveTimestamp, reply);
}

void MessageModel::sendFileMessage(const quint64 id, const QString& serverId,
                                   const QString& contactPubKey,
                                   const QString& text,
                                   const QByteArray& content, quint64 timestamp,
                                   const ReplyInfo& reply) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;
  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
  const QString timeString = formatMessageTime(effectiveTimestamp);
  addMessage(id, text, timeString, true, true, false, false, false, false,
             false, MESSAGE_TYPE::FILE, content, effectiveTimestamp, reply);
}

void MessageModel::receiveFileMessage(const quint64 id, const QString& serverId,
                                      const QString& contactPubKey,
                                      const QString& text,
                                      const QByteArray& content,
                                      quint64 timestamp,
                                      const ReplyInfo& reply) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;
  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
  const QString timeString = formatMessageTime(effectiveTimestamp);
  addMessage(id, text, timeString, false, true, false, false, false, false,
             false, MESSAGE_TYPE::FILE, content, effectiveTimestamp, reply);
}

void MessageModel::addPendingFileMessage(
    const quint64 id, const QString& serverId, const QString& contactPubKey,
    const QString& text, const QByteArray& content, quint64 timestamp,
    const ReplyInfo& reply) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
  const QString timeString = formatMessageTime(effectiveTimestamp);
  addMessage(id, text, timeString, true, true, false, false, false, false,
             false, MESSAGE_TYPE::FILE, content, effectiveTimestamp, reply);

  const int messageIndex = findMessageIndex(id);
  if (messageIndex >= 0) {
    m_messages[messageIndex].fileTransferPending = true;
    m_messages[messageIndex].fileTransferCancelable = true;
    m_messages[messageIndex].fileTransferProgress = 0.0;
    m_messages[messageIndex].fileTransferStatusText = tr("Uploading 0%");
    emit dataChanged(index(messageIndex), index(messageIndex),
                     {FileTransferPendingRole, FileTransferCancelableRole,
                      FileTransferProgressRole, FileTransferStatusTextRole});
  }
}

void MessageModel::sendAudioMessage(const quint64 id, const QString& serverId,
                                    const QString& contactPubKey,
                                    const QString& text,
                                    const QByteArray& content, quint64 timestamp,
                                    const ReplyInfo& reply) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;
  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
  const QString timeString = formatMessageTime(effectiveTimestamp);
  addMessage(id, text, timeString, true, true, false, false, false, false,
             false, MESSAGE_TYPE::AUDIO, content, effectiveTimestamp, reply);
}

void MessageModel::receiveAudioMessage(
    const quint64 id, const QString& serverId, const QString& contactPubKey,
    const QString& text, const QByteArray& content, quint64 timestamp,
    const ReplyInfo& reply) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;
  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
  const QString timeString = formatMessageTime(effectiveTimestamp);
  addMessage(id, text, timeString, false, true, false, false, false, false,
             false, MESSAGE_TYPE::AUDIO, content, effectiveTimestamp, reply);
}

void MessageModel::addPendingAudioMessage(
    const quint64 id, const QString& serverId, const QString& contactPubKey,
    const QString& text, const QByteArray& content, quint64 timestamp,
    const ReplyInfo& reply) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
  const QString timeString = formatMessageTime(effectiveTimestamp);
  addMessage(id, text, timeString, true, true, false, false, false, false,
             false, MESSAGE_TYPE::AUDIO, content, effectiveTimestamp, reply);

  const int messageIndex = findMessageIndex(id);
  if (messageIndex >= 0) {
    m_messages[messageIndex].fileTransferPending = true;
    m_messages[messageIndex].fileTransferCancelable = true;
    m_messages[messageIndex].fileTransferProgress = 0.0;
    m_messages[messageIndex].fileTransferStatusText = tr("Uploading 0%");
    emit dataChanged(index(messageIndex), index(messageIndex),
                     {FileTransferPendingRole, FileTransferCancelableRole,
                      FileTransferProgressRole, FileTransferStatusTextRole});
  }
}

void MessageModel::updateMessageTimestamp(const QString& serverId,
                                          const QString& contactPubKey,
                                          const quint64 id, quint64 timestamp) {
  if (timestamp == 0 || serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  const int messageIndex = findMessageIndex(id);
  if (messageIndex < 0) {
    return;
  }

  m_messages[messageIndex].t = timestamp;
  m_messages[messageIndex].time = formatMessageTime(timestamp);
  emit dataChanged(index(messageIndex), index(messageIndex), {TimeRole});
}

void MessageModel::updateMessageSendedStatus(const QString& serverId,
                                             const QString& contactPubKey,
                                             const quint64 id) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  const int messageIndex = findMessageIndex(id);
  if (messageIndex >= 0) {
    m_messages[messageIndex].isSended = true;
    emit messageStatusUpdated(messageIndex);
    emit dataChanged(index(messageIndex), index(messageIndex), {IsSendedRole});
  }
}

void MessageModel::updateMessageSentStatus(const QString& serverId,
                                           const QString& contactPubKey,
                                           const quint64 id) {
  updateMessageSendedStatus(serverId, contactPubKey, id);
}

void MessageModel::updateMessageDeliveredStatus(const QString& serverId,
                                                const QString& contactPubKey,
                                                const quint64 id) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  const int messageIndex = findMessageIndex(id);
  if (messageIndex >= 0) {
    m_messages[messageIndex].isReceived = true;
    emit messageStatusUpdated(messageIndex);
    emit dataChanged(index(messageIndex), index(messageIndex),
                     {IsReceivedRole});
  }
}

void MessageModel::updateCallDuration(const QString& serverId,
                                      const QString& contactPubKey,
                                      const quint64 id, quint64 durationSec) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  const int messageIndex = findMessageIndex(id);
  if (messageIndex < 0) {
    return;
  }

  m_messages[messageIndex].callDurationSec = durationSec;
  emit dataChanged(index(messageIndex), index(messageIndex),
                   {CallDurationRole});
}

void MessageModel::addCallStatusMessageIfCurrentChat(
    const QString& serverId, const QString& contactPubKey, const quint64 id,
    const QString& text, bool isOwn) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  addMessage(id, text, QDateTime::currentDateTime().toString("HH:mm"), isOwn,
             true, false, true, true, true, true);
}

void MessageModel::updateFileLocalPath(const QString& serverId,
                                       const QString& contactPubKey,
                                       const quint64 id,
                                       const QString& localPath) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  const int messageIndex = findMessageIndex(id, true);
  if (messageIndex >= 0) {
    m_messages[messageIndex].fileData.localPath = localPath;
    ensureAudioWaveform(&m_messages[messageIndex].fileData);
    emit dataChanged(
        index(messageIndex), index(messageIndex),
        {FileLocalPathRole, FileDownloadedRole, AudioWaveformRole});
  }
}

void MessageModel::updateFileTransferProgress(const quint64 id,
                                              qreal progress) {
  const qreal bounded = qBound<qreal>(0.0, progress, 1.0);

  const int messageIndex = findMessageIndex(id, true);
  if (messageIndex >= 0) {
    if (qFuzzyCompare(m_messages[messageIndex].fileTransferProgress, bounded)) {
      return;
    }

    m_messages[messageIndex].fileTransferProgress = bounded;
    emit dataChanged(index(messageIndex), index(messageIndex),
                     {FileTransferProgressRole});
  }
}

void MessageModel::setFileTransferState(const QString& serverId,
                                        const QString& contactPubKey,
                                        const quint64 id, bool pending,
                                        bool cancelable, qreal progress,
                                        const QString& statusText) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  const qreal bounded = qBound<qreal>(0.0, progress, 1.0);

  const int messageIndex = findMessageIndex(id, true);
  if (messageIndex >= 0) {
    m_messages[messageIndex].fileTransferPending = pending;
    m_messages[messageIndex].fileTransferCancelable = cancelable;
    m_messages[messageIndex].fileTransferProgress = bounded;
    m_messages[messageIndex].fileTransferStatusText = statusText;
    emit dataChanged(index(messageIndex), index(messageIndex),
                     {FileTransferPendingRole, FileTransferCancelableRole,
                      FileTransferProgressRole, FileTransferStatusTextRole});
  }
}

void MessageModel::completePendingFileMessage(const QString& serverId,
                                              const QString& contactPubKey,
                                              const quint64 id,
                                              const QByteArray& content) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  const int messageIndex = findMessageIndex(id, true);
  if (messageIndex >= 0) {
    filemessage::deserialize(content, &m_messages[messageIndex].fileData);
    ensureAudioWaveform(&m_messages[messageIndex].fileData);
    m_messages[messageIndex].fileTransferPending = false;
    m_messages[messageIndex].fileTransferCancelable = false;
    m_messages[messageIndex].fileTransferProgress = 1.0;
    m_messages[messageIndex].fileTransferStatusText.clear();
    emit dataChanged(index(messageIndex), index(messageIndex),
                     {FileNameRole, FileSizeRole, IsImageRole, IsVideoRole,
                      FileMediaTypeRole, FileMimeTypeRole, AudioDurationRole,
                      AudioWaveformRole, FileLocalPathRole, FileDownloadedRole,
                      FileTransferPendingRole, FileTransferCancelableRole,
                      FileTransferProgressRole, FileTransferStatusTextRole});
  }
}

void MessageModel::receiveAlbumMessage(
    const quint64 id, const QString& serverId, const QString& contactPubKey,
    const QString& caption, const QByteArray& content, quint64 timestamp,
    const ReplyInfo& reply) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;
  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
  const QString timeString = formatMessageTime(effectiveTimestamp);
  addMessage(id, caption, timeString, false, true, false, false, false, false,
             false, MESSAGE_TYPE::ALBUM, content, effectiveTimestamp, reply);
}

void MessageModel::addPendingAlbumMessage(
    const quint64 id, const QString& serverId, const QString& contactPubKey,
    const QString& caption, const QByteArray& content, quint64 timestamp,
    const ReplyInfo& reply) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;
  const quint64 effectiveTimestamp =
      timestamp > 0 ? timestamp
                    : static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
  const QString timeString = formatMessageTime(effectiveTimestamp);
  addMessage(id, caption, timeString, true, true, false, false, false, false,
             false, MESSAGE_TYPE::ALBUM, content, effectiveTimestamp, reply);

  const int messageIndex = findMessageIndex(id);
  if (messageIndex >= 0) {
    m_messages[messageIndex].fileTransferPending = true;
    m_messages[messageIndex].fileTransferCancelable = false;
    m_messages[messageIndex].fileTransferProgress = 0.0;
    m_messages[messageIndex].fileTransferStatusText = tr("Uploading 0%");
    emit dataChanged(index(messageIndex), index(messageIndex),
                     {FileTransferPendingRole, FileTransferCancelableRole,
                      FileTransferProgressRole, FileTransferStatusTextRole});
  }
}

void MessageModel::completePendingAlbumMessage(const QString& serverId,
                                               const QString& contactPubKey,
                                               const quint64 id,
                                               const QByteArray& content) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;
  const int messageIndex = findMessageIndex(id);
  if (messageIndex >= 0) {
    filemessage::deserializeAlbum(content, &m_messages[messageIndex].albumData);
    const int itemCount = m_messages[messageIndex].albumData.items.size();
    m_messages[messageIndex].albumItemDownloading = QList<bool>(itemCount, false);
    m_messages[messageIndex].albumItemProgress = QList<qreal>(itemCount, 0.0);
    m_messages[messageIndex].fileTransferPending = false;
    m_messages[messageIndex].fileTransferCancelable = false;
    m_messages[messageIndex].fileTransferProgress = 1.0;
    m_messages[messageIndex].fileTransferStatusText.clear();
    emit dataChanged(index(messageIndex), index(messageIndex),
                     {AlbumItemsRole, AlbumProgressRole, FileTransferPendingRole,
                      FileTransferCancelableRole, FileTransferProgressRole,
                      FileTransferStatusTextRole});
  }
}

void MessageModel::updateAlbumItemLocalPath(const QString& serverId,
                                            const QString& contactPubKey,
                                            const quint64 id, int itemIndex,
                                            const QString& localPath) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;
  const int messageIndex = findMessageIndex(id);
  if (messageIndex < 0) {
    return;
  }
  Message& message = m_messages[messageIndex];
  if (itemIndex < 0 || itemIndex >= message.albumData.items.size()) {
    return;
  }
  message.albumData.items[itemIndex].localPath = localPath;
  if (itemIndex < message.albumItemDownloading.size()) {
    message.albumItemDownloading[itemIndex] = false;
  }
  if (itemIndex < message.albumItemProgress.size()) {
    message.albumItemProgress[itemIndex] = 1.0;
  }
  emit dataChanged(index(messageIndex), index(messageIndex),
                   {AlbumItemsRole, AlbumProgressRole});
}

void MessageModel::setAlbumItemDownloading(const QString& serverId,
                                           const QString& contactPubKey,
                                           const quint64 id, int itemIndex,
                                           bool downloading, qreal progress) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;
  const int messageIndex = findMessageIndex(id);
  if (messageIndex < 0) {
    return;
  }
  Message& message = m_messages[messageIndex];
  if (itemIndex < 0 || itemIndex >= message.albumData.items.size()) {
    return;
  }
  const qreal bounded = qBound<qreal>(0.0, progress, 1.0);
  bool changed = false;
  if (itemIndex < message.albumItemDownloading.size() &&
      message.albumItemDownloading[itemIndex] != downloading) {
    message.albumItemDownloading[itemIndex] = downloading;
    changed = true;
  }
  if (itemIndex < message.albumItemProgress.size()) {
    // Throttle to whole-percent changes so a fast (local) server can't flood
    // dataChanged and stall the UI thread.
    const int oldPct = qRound(message.albumItemProgress[itemIndex] * 100.0);
    const int newPct = qRound(bounded * 100.0);
    if (oldPct != newPct) {
      message.albumItemProgress[itemIndex] = bounded;
      changed = true;
    }
  }
  if (changed) {
    emit dataChanged(index(messageIndex), index(messageIndex),
                     {AlbumProgressRole});
  }
}

void MessageModel::removeTransientMessage(const QString& serverId,
                                          const QString& contactPubKey,
                                          const quint64 id) {
  deleteMessage(serverId, contactPubKey, id);
}

void MessageModel::markMessagesAsReaded(const QString& serverId,
                                        const QString& contactPubKey) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;

  for (int i = 0; i < m_messages.size(); ++i) {
    if (m_messages[i].isMsg && (!m_messages[i].isOwn) &&
        !m_messages[i].isReaded) {
      m_messages[i].isReaded = true;
      emit messageStatusUpdated(i);
      emit dataChanged(index(i), index(i), {IsReadedRole});
    }
  }
}

void MessageModel::markMessagesAsRead(const QString& serverId,
                                      const QString& contactPubKey) {
  markMessagesAsReaded(serverId, contactPubKey);
}

void MessageModel::markOwnMessagesAsReaded(const QString& serverId,
                                           const QString& contactPubKey) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey)
    return;

  for (int i = 0; i < m_messages.size(); ++i) {
    if (m_messages[i].isMsg && (m_messages[i].isOwn) &&
        !m_messages[i].isReaded) {
      m_messages[i].isReaded = true;
      emit messageStatusUpdated(i);
      emit dataChanged(index(i), index(i), {IsReadedRole});
    }
  }
}

void MessageModel::markOwnMessagesAsRead(const QString& serverId,
                                         const QString& contactPubKey) {
  markOwnMessagesAsReaded(serverId, contactPubKey);
}

bool MessageModel::hasUnreadMessages(const QString& serverId,
                                     const QString& contactPubKey) {
  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return false;
  }

  for (int i = 0; i < m_messages.size(); ++i) {
    if (m_messages[i].isMsg && !m_messages[i].isOwn &&
        !m_messages[i].isReaded) {
      return true;
    }
  }
  return false;
}

bool MessageModel::isCurrentChat(const QString& serverId,
                                 const QString& contactPubKey) const {
  return serverId == m_currentContactServer &&
         contactPubKey == m_currentContactPubKey;
}

void MessageModel::closeCurrentChat() {
  m_currentContactServer.clear();
  m_currentContactPubKey.clear();
  clear();
}

quint64 MessageModel::uniqId() {
  // Per-message id, unique within a conversation. The PK is
  // (serverAddress, fromPubKey, id); the previous scheme used only 16 random
  // bits within a one-second window, so an id could occasionally collide with
  // an existing message (own or the peer's) — the INSERT then failed and the
  // message never persisted, even though the contact's last-message preview
  // (stored separately) still updated. Messages are ordered by their timestamp
  // (`t`), not by id, so the id can be fully random: 48 random bits keep it well
  // within JS/QML's safe-integer range (< 2^53) while making collisions
  // astronomically unlikely.
  quint64 id = QRandomGenerator::global()->generate64() & 0xFFFFFFFFFFFFULL;
  return id == 0 ? 1 : id;
}

void MessageModel::deleteMessage(const QString& serverId,
                                 const QString& contactPubKey,
                                 const quint64 id) {
  // bool lastDeleted = false;

  if (serverId != m_currentContactServer ||
      contactPubKey != m_currentContactPubKey) {
    return;
  }

  const int messageIndex = findMessageIndex(id);
  if (messageIndex >= 0) {
    if (messageIndex > 0 && m_messages[messageIndex - 1].isDate) {
      beginRemoveRows(QModelIndex(), messageIndex, messageIndex);
      m_messages.removeAt(messageIndex);
      endRemoveRows();
      beginRemoveRows(QModelIndex(), messageIndex - 1, messageIndex - 1);
      m_messages.removeAt(messageIndex - 1);
      endRemoveRows();
    } else {
      beginRemoveRows(QModelIndex(), messageIndex, messageIndex);
      m_messages.removeAt(messageIndex);
      endRemoveRows();
    }
  }

  if (m_messages.isEmpty()) {
    m_contactsModel->setLastMessage(serverId, contactPubKey, "", "");
    return;
  }

  const int lastMessageIndex = findLastNonDateMessageIndex();
  if (lastMessageIndex >= 0) {
    m_contactsModel->setLastMessage(serverId, contactPubKey,
                                    m_messages[lastMessageIndex].text,
                                    m_messages[lastMessageIndex].time);
  }
}

void MessageModel::onMessageDelete(const quint64 message_id,
                                   const QString& serverId,
                                   const QString& contactPubKey) {
  deleteMessage(serverId, contactPubKey, message_id);
}