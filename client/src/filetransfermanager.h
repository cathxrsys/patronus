#pragma once

#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

#include "filemessageutils.h"
#include "messagemodel.h"

class QNetworkReply;
class QNetworkRequest;
class QSaveFile;
class QThread;

class ConnectionManager;
class DatabaseManager;
class ContactsModel;
class SettingsManager;

struct BlobPreparationResult {
  bool success = false;
  QString encryptedPath;
  QString errorText;
  FileMessageData fileData;
};

using BlobProgressCallback =
    std::function<void(quint64 processedBytes, quint64 totalBytes)>;

class BlobDownloadWorker : public QObject {
  Q_OBJECT

 public:
  explicit BlobDownloadWorker(const QString& urlString,
                              const QString& accessToken, bool ignoreSslErrors,
                              const QString& finalPath, const QByteArray& key,
                              quint64 expectedFileSize,
                              QObject* parent = nullptr);
  ~BlobDownloadWorker() override;

 public slots:
  void initialize();
  void cancel();

 signals:
  void progressUpdated(quint64 processedBytes, quint64 totalBytes);
  void failed(const QString& errorText);
  void completed();
  void canceled();

 private:
  void startRequest();
  void handleReadyRead();
  void handleFinished();
  bool parseBlobHeader();
  bool processBufferedData();
  void failInternal(const QString& errorText);
  void cleanupOutputFile(bool commit);

  QString m_urlString;
  QString m_accessToken;
  bool m_ignoreSslErrors = false;
  QString m_finalPath;
  QByteArray m_key;
  QByteArray m_buffer;
  QByteArray m_noncePrefix;
  quint64 m_expectedFileSize = 0;
  quint64 m_originalSize = 0;
  quint64 m_bytesRemaining = 0;
  quint64 m_processedBytes = 0;
  quint64 m_chunkIndex = 0;
  quint32 m_chunkSize = 0;
  bool m_headerParsed = false;
  bool m_done = false;
  QNetworkAccessManager* m_networkManager = nullptr;
  QNetworkReply* m_reply = nullptr;
  QSaveFile* m_outputFile = nullptr;
};

class FileTransferManager : public QObject {
  Q_OBJECT

 public:
  explicit FileTransferManager(ConnectionManager* connectionManager,
                               DatabaseManager* databaseManager,
                               MessageModel* messageModel,
                               ContactsModel* contactsModel,
                               SettingsManager* settingsManager,
                               QObject* parent = nullptr);

  Q_INVOKABLE void selectAndSendFile(const QString& serverId,
                                     const QString& contactPubKey);
  Q_INVOKABLE void selectAndSendMedia(const QString& serverId,
                                      const QString& contactPubKey);
  // Opens the native multi-select picker and, once files are chosen, emits
  // filesPicked() with a classified descriptor per file so QML can stage them
  // above the input instead of sending immediately. mediaOnly restricts the
  // filter to images/videos.
  Q_INVOKABLE void pickFilesForStaging(const QString& serverId,
                                       const QString& contactPubKey,
                                       bool mediaOnly);
  // Lightweight classification (name, size, media/mime type, kind flags) of a
  // local path, used to build staging descriptors.
  Q_INVOKABLE QVariantMap classifyFile(const QString& filePath) const;
  // Sends one or more staged files as a single album message with an optional
  // caption. Each file is encrypted and uploaded independently; the album
  // message is emitted once every item has uploaded.
  Q_INVOKABLE void sendAlbum(const QString& serverId,
                             const QString& contactPubKey,
                             const QStringList& filePaths,
                             const QString& caption, quint64 replyTo,
                             const QString& replyPreview,
                             const QString& replyKind, bool replyIsOwn);
  // Downloads a single item of a received album message.
  Q_INVOKABLE void downloadAlbumItem(const QString& serverId,
                                     const QString& contactPubKey,
                                     quint64 messageId, int itemIndex);
  // Saves a message's file(s) to the user's Downloads folder, downloading first
  // if needed. For each file it ultimately emits saveToDownloadsRequested with a
  // ready local path (QML performs the platform-specific copy into Downloads).
  Q_INVOKABLE void downloadToDownloads(const QString& serverId,
                                       const QString& contactPubKey,
                                       quint64 messageId);
  // Desktop copy of a local file into the Downloads folder; returns true on
  // success. (Android saves via MediaStore from QML instead.)
  Q_INVOKABLE bool copyToDownloads(const QString& localPath,
                                   const QString& displayName);
  Q_INVOKABLE void sendAudioMessage(const QString& serverId,
                                    const QString& contactPubKey,
                                    const QString& filePath, quint64 durationMs,
                                    const QList<int>& waveform,
                                    const ReplyInfo& reply = {});
  Q_INVOKABLE void downloadFile(const QString& serverId,
                                const QString& contactPubKey,
                                quint64 messageId);
  Q_INVOKABLE QString localFileUrl(const QString& localPath) const;
  Q_INVOKABLE void openLocalFile(const QString& localPath);
  Q_INVOKABLE void openContainingFolder(const QString& localPath);
  Q_INVOKABLE void cancelUpload(const QString& serverId,
                                const QString& contactPubKey,
                                quint64 messageId);
  Q_INVOKABLE void cancelDownload(const QString& serverId,
                                  const QString& contactPubKey,
                                  quint64 messageId);

  void retryPendingFileUploads(const QString& serverId);
  // Resends own album messages still marked isSended = 0 (never delivered:
  // either the initial e2eSendJSON failed after every file had already
  // uploaded, or the app dropped connection before an ack arrived). Rebuilds
  // the wire message from the locally stored content — the files themselves
  // are already on the server, so this never re-uploads anything.
  void retryPendingAlbumMessages(const QString& serverId);
  // Reconnect handler for albums composed while offline: re-drives the queued
  // per-item blob uploads from the pending_album_uploads store and assembles the
  // album wire message once every item is on the server. Complements
  // retryPendingAlbumMessages (which only resends an already-uploaded album's
  // wire message).
  void retryPendingAlbumUploads(const QString& serverId);
  void onChatOpened(const QString& serverId, const QString& contactPubKey);
  void onMessageDeleteRequested(quint64 messageId, const QString& serverId,
                                 const QString& contactPubKey);

 signals:
  void transferError(const QString& errorText);
  void transferFinished(const QString& messageText);
  // Emitted after pickFilesForStaging resolves; `items` is a list of QVariantMap
  // descriptors (as produced by classifyFile plus a "path" key).
  void filesPicked(const QString& serverId, const QString& contactPubKey,
                   const QVariantList& items);
  // A file is ready on disk and should be copied into the Downloads folder.
  void saveToDownloadsRequested(const QString& localPath,
                                const QString& fileName);

 private:
  // Coordinates the N per-file uploads that make up one album message. Each
  // item's FileMessageData is filled in as its upload finalizes; when all
  // items are done the album wire message + DB row are emitted.
  struct AlbumUpload {
    QString serverId;
    QString contactPubKey;
    quint64 messageId = 0;
    quint64 timestamp = 0;
    QString caption;
    ReplyInfo reply;
    int totalItems = 0;
    int completedItems = 0;
    int outstanding = 0;  // item uploads not yet released; album freed at 0
    int deferredItems = 0;  // items queued (offline) rather than uploaded
    bool failed = false;
    // True once the album has a pending_album_uploads row (queued offline);
    // keeps a mid-retry disconnect from destroying it.
    bool persisted = false;
    QList<FileMessageData> items;  // sized to totalItems, filled by index
    // Both sized to totalItems. deferredPaths holds the encrypted blob path for
    // items still needing upload (empty once uploaded); itemMessageIds keeps the
    // per-item transfer key stable across reconnect retries.
    QStringList deferredPaths;
    QList<quint64> itemMessageIds;
  };

  struct PendingUpload {
    QString serverId;
    QString contactPubKey;
    QString previewText;
    QString encryptedPath;
    QString sourcePath;
    QString uploadUrl;
    QString accessToken;
    quint64 messageId = 0;
    quint64 timestamp = 0;
    FileMessageData fileData;
    MessageModel::MESSAGE_TYPE messageType = MessageModel::MESSAGE_TYPE::FILE;
    QNetworkReply* reply = nullptr;
    QFutureWatcher<BlobPreparationResult>* watcher = nullptr;
    bool canceled = false;
    // When part of an album: the coordinator and this item's slot. albumIndex
    // < 0 means a standalone (non-album) upload.
    AlbumUpload* album = nullptr;
    int albumIndex = -1;
    QString uploadKey;  // key under which this upload lives in m_activeUploads
    ReplyInfo replyInfo;  // set when this standalone upload is a reply (e.g. voice)
  };

  struct PendingBlobDownload {
    QString serverId;
    QString contactPubKey;
    QString finalPath;
    quint64 messageId = 0;
    FileMessageData fileData;
    QThread* workerThread = nullptr;
    BlobDownloadWorker* worker = nullptr;
    bool canceled = false;
    // When downloading a single album item: this item's slot and the key it
    // lives under in m_activeDownloads. albumIndex < 0 means a whole-file
    // download.
    int albumIndex = -1;
    QString downloadKey;
  };

  struct PendingMessageStatus {
    QString serverId;
    QString contactPubKey;
    quint64 messageId = 0;
  };

  void sendFile(const QString& serverId, const QString& contactPubKey,
                const QString& filePath);
  void finalizeAlbum(AlbumUpload* album);
  void failAlbum(AlbumUpload* album, const QString& errorText);
  void updateAlbumUploadProgress(AlbumUpload* album);
  // Called once per album item when its upload object is finally released;
  // frees the coordinator when every item is accounted for and it has failed,
  // or persists the album for later when items were queued offline.
  void albumItemDone(AlbumUpload* album);
  // Writes (or rewrites) the album's pending_album_uploads row: the encrypted
  // blobs still to upload plus the metadata needed to assemble the album on
  // reconnect. Called when queuing offline and after each item uploads during a
  // retry so a fresh disconnect resumes from the last saved point.
  void persistAlbumUpload(AlbumUpload* album);
  void sendBlobMessage(const QString& serverId, const QString& contactPubKey,
                       const QString& filePath,
                       MessageModel::MESSAGE_TYPE messageType,
                       const FileMessageData& initialFileData,
                       AlbumUpload* album = nullptr, int albumIndex = -1,
                       const ReplyInfo& reply = {});
  void startUploadRequest(PendingUpload* upload);
  void finalizeUpload(PendingUpload* upload, const QByteArray& responseBody);
  void failUpload(PendingUpload* upload, const QString& errorText);
  void removePendingBubble(PendingUpload* upload);
  void releaseUpload(PendingUpload* upload);

  void onRequestStateChanged(uint32_t requestId, const QString& status);
  // Auto-downloads a freshly received file/audio/album per the user's
  // Auto-download settings (media / files / voice).
  void onMediaMessageReceived(const QString& serverId,
                              const QString& fromPubKey, quint64 messageId,
                              bool isAlbum);
  bool autoDownloadEnabledFor(const FileMessageData& fileData) const;

  QString filesDirectory() const;
  QString makeUniqueTargetPath(const QString& preferredName) const;
  QString sanitizeFileName(const QString& fileName) const;
  QString buildFilesUrl(const QString& serverId,
                        const QString& fileId = QString()) const;
  void applySslPolicy(QNetworkReply* reply) const;
  // Proactively disables certificate verification on the outgoing request
  // (rather than reacting to sslErrors on the reply) when ignoreSslErrors is
  // on - see the call sites for why the reactive-only approach was not
  // reliably bypassing a self-signed/hostname-mismatched cert over plain
  // QNetworkAccessManager requests.
  void applySslPolicy(QNetworkRequest& request) const;
  QString transferKey(const QString& serverId, const QString& contactPubKey,
                      quint64 messageId) const;
  void updateMessageLocalPath(const QString& serverId,
                              const QString& contactPubKey, quint64 messageId,
                              const QString& localPath);
  void updateAlbumItemLocalPath(const QString& serverId,
                                const QString& contactPubKey, quint64 messageId,
                                int itemIndex, const QString& localPath);
  void setTransferState(const QString& serverId, const QString& contactPubKey,
                        quint64 messageId, qreal progress,
                        const QString& statusText);
  void setTransferProgress(const QString& serverId,
                           const QString& contactPubKey, quint64 messageId,
                           const QString& actionText, qreal progress);
  void postInfoMessage(const QString& serverId, const QString& contactPubKey,
                       const QString& text);

  void cleanupDownload(PendingBlobDownload* download);
  void failDownload(PendingBlobDownload* download, const QString& errorText);
  void finishDownload(PendingBlobDownload* download);
  void clearTransferState(const QString& serverId, const QString& contactPubKey,
                          quint64 messageId);

  QNetworkAccessManager m_networkManager;
  ConnectionManager* m_connectionManager;
  DatabaseManager* m_db;
  MessageModel* m_messageModel;
  ContactsModel* m_contactsModel;
  SettingsManager* m_settingsManager;
  QHash<QNetworkReply*, PendingUpload*> m_uploads;
  QHash<QString, PendingUpload*> m_activeUploads;
  QHash<QString, PendingBlobDownload*> m_activeDownloads;
  QHash<quint64, AlbumUpload*> m_activeAlbums;
  QHash<uint32_t, PendingMessageStatus> m_pendingStatuses;
  // Guards retryPendingAlbumMessages against re-sending the same row twice if
  // two reconnects happen before the first resend's delivery ack arrives.
  QSet<quint64> m_pendingRetryAlbumMessageIds;
  // Download keys whose file should be copied to Downloads once fetched.
  QSet<QString> m_saveAfterDownload;
};