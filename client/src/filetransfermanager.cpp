#include "filetransfermanager.h"

#include <sodium.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QUrl>
#include <QtConcurrentRun>
#include <QtEndian>

#include "apppaths.h"
#include "connectionmanager.h"
#include "contactsmodel.h"
#include "databasemanager.h"
#include "mainsignals.h"
#include "fileopener.h"
#include "filepicker.h"
#include "messagemodel.h"
#include "settingsmanager.h"
#include "voicecliputils.h"

namespace {

constexpr quint32 kFileChunkSize = 64 * 1024;
constexpr int kTagSize = crypto_aead_chacha20poly1305_ietf_ABYTES;
constexpr int kKeySize = crypto_aead_chacha20poly1305_ietf_KEYBYTES;
constexpr int kNonceSize = crypto_aead_chacha20poly1305_ietf_NPUBBYTES;
constexpr int kNoncePrefixSize = 4;
constexpr int kHeaderSize = 8 + 1 + 4 + 8 + kNoncePrefixSize;
const QByteArray kBlobMagic("ARCFILE1", 8);
const quint8 kBlobVersion = 1;
const QString kEncryptionName =
    QStringLiteral("chacha20poly1305-ietf-chunked-v1");

QByteArray makeNonce(const QByteArray& prefix, quint64 chunkIndex) {
  QByteArray nonce(kNonceSize, Qt::Uninitialized);
  memcpy(nonce.data(), prefix.constData(), kNoncePrefixSize);

  quint64 beIndex = qToBigEndian(chunkIndex);
  memcpy(nonce.data() + kNoncePrefixSize, &beIndex, sizeof(beIndex));
  return nonce;
}

QString networkErrorText(QNetworkReply* reply, bool isUpload) {
  if (reply == nullptr) {
    return QStringLiteral("Network request failed");
  }

  const QVariant statusCode =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
  const QVariant reasonPhrase =
      reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute);
  QStringList details;

  if (statusCode.isValid()) {
    QString statusText = QString::number(statusCode.toInt());
    const QString reasonText = reasonPhrase.toString().trimmed();
    if (!reasonText.isEmpty()) {
      statusText += QLatin1Char(' ') + reasonText;
    }

    // The server returns 403 only for an authenticated-but-not-yet-a-member
    // session (see requireSessionToken); it never means the blob is missing.
    if (statusCode.toInt() == 403) {
      return QObject::tr(
          "File transfer is not available for your account on this server "
          "yet. This usually resolves itself once you reconnect; otherwise "
          "ask your contact to add you again");
    }

    if (statusCode.toInt() == 404) {
      // An upload targets a brand-new blob (no id in the URL), so a 404
      // here can never mean "the file does not exist" - it only happens
      // when the session/token used for the request was rejected.
      if (isUpload) {
        return QObject::tr(
            "Upload failed. Your session may have expired - try "
            "reconnecting and sending again");
      }

      return QObject::tr(
          "The file does not exist. Ask your contact to send it again");
    }

    details << QObject::tr("HTTP %1").arg(statusText);
  }

  const QString message = reply->errorString().trimmed();
  if (!message.isEmpty()) {
    details << message;
  }

  QByteArray body;
  if (reply->isOpen()) {
    body = reply->peek(512).trimmed();
  }

  if (!body.isEmpty()) {
    QString bodyText = QString::fromUtf8(body).trimmed();
    if (!bodyText.isEmpty()) {
      bodyText.replace('\n', ' ');
      details << QObject::tr("Response: %1").arg(bodyText);
    }
  }

  if (!details.isEmpty()) {
    return details.join(QStringLiteral(" | "));
  }

  return QStringLiteral("Network request failed");
}

QString detectMediaType(const QString& filePath, const QString& mimeType) {
  if (mimeType.startsWith(QStringLiteral("image/"))) {
    return QStringLiteral("image");
  }

  if (mimeType.startsWith(QStringLiteral("video/"))) {
    return QStringLiteral("video");
  }

  const QString suffix = QFileInfo(filePath).suffix().toLower();
  static const QStringList imageExtensions = {
      QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
      QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp")};
  static const QStringList videoExtensions = {
      QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("webm"),
      QStringLiteral("avi"), QStringLiteral("mov"), QStringLiteral("m4v")};

  if (imageExtensions.contains(suffix)) {
    return QStringLiteral("image");
  }

  if (videoExtensions.contains(suffix)) {
    return QStringLiteral("video");
  }

  return QString();
}

}  // namespace

BlobDownloadWorker::BlobDownloadWorker(
    const QString& urlString, const QString& accessToken, bool ignoreSslErrors,
    const QString& finalPath, const QByteArray& key, quint64 expectedFileSize,
    QObject* parent)
    : QObject(parent),
      m_urlString(urlString),
      m_accessToken(accessToken),
      m_ignoreSslErrors(ignoreSslErrors),
      m_finalPath(finalPath),
      m_key(key),
      m_expectedFileSize(expectedFileSize) {}

BlobDownloadWorker::~BlobDownloadWorker() { cleanupOutputFile(false); }

void BlobDownloadWorker::initialize() {
  if (m_done) {
    return;
  }

  m_outputFile = new QSaveFile(m_finalPath);
  if (!m_outputFile->open(QIODevice::WriteOnly)) {
    failInternal(tr("Cannot create output file"));
    return;
  }

  startRequest();
}

void BlobDownloadWorker::cancel() {
  if (m_done) {
    return;
  }

  m_done = true;
  if (m_reply != nullptr) {
    m_reply->abort();
  }
  cleanupOutputFile(false);
  emit canceled();
}

void BlobDownloadWorker::startRequest() {
  if (m_done) {
    return;
  }

  m_networkManager = new QNetworkAccessManager(this);

  QNetworkRequest request{QUrl(m_urlString)};
  request.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
  if (m_ignoreSslErrors) {
    // Disable verification up front rather than relying solely on reacting to
    // sslErrors below: a self-signed/hostname-mismatched cert on a plain
    // QNetworkAccessManager request has been observed to hard-fail the
    // handshake (SslHandshakeFailedError) before/without that signal ever
    // giving ignoreSslErrors() a chance to take effect, unlike the primary
    // websocket connection which tolerates the same cert fine.
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(sslConfig);
  }

  m_reply = m_networkManager->get(request);
  if (m_ignoreSslErrors) {
    connect(m_reply, &QNetworkReply::sslErrors, m_reply,
            [reply = m_reply](const QList<QSslError>&) {
              reply->ignoreSslErrors();
            });
  }

  connect(m_reply, &QNetworkReply::readyRead, this,
          [this]() { handleReadyRead(); });

  connect(m_reply, &QNetworkReply::finished, this,
          [this]() { handleFinished(); });
}

void BlobDownloadWorker::handleReadyRead() {
  if (m_done || m_reply == nullptr) {
    return;
  }

  m_buffer.append(m_reply->readAll());

  if (!m_headerParsed && !parseBlobHeader()) {
    return;
  }

  processBufferedData();
}

void BlobDownloadWorker::handleFinished() {
  if (m_reply == nullptr) {
    return;
  }

  if (m_done && m_reply->error() == QNetworkReply::OperationCanceledError) {
    m_reply->deleteLater();
    m_reply = nullptr;
    return;
  }

  if (m_reply->error() != QNetworkReply::NoError) {
    const QString errorText = networkErrorText(m_reply, /*isUpload=*/false);
    m_reply->deleteLater();
    m_reply = nullptr;
    failInternal(errorText);
    return;
  }

  m_buffer.append(m_reply->readAll());
  m_reply->deleteLater();
  m_reply = nullptr;

  if (!m_headerParsed && !parseBlobHeader()) {
    failInternal(tr("Downloaded file is incomplete"));
    return;
  }

  if (!processBufferedData()) {
    return;
  }

  if (!m_headerParsed || m_bytesRemaining != 0 || !m_buffer.isEmpty()) {
    failInternal(tr("Downloaded file is incomplete"));
    return;
  }

  m_done = true;
  if (m_outputFile == nullptr || !m_outputFile->commit()) {
    cleanupOutputFile(false);
    emit failed(tr("Failed to finalize the downloaded file"));
    return;
  }

  cleanupOutputFile(true);
  emit completed();
}

bool BlobDownloadWorker::parseBlobHeader() {
  if (m_buffer.size() < kHeaderSize) {
    return false;
  }

  const QByteArray header = m_buffer.left(kHeaderSize);
  m_buffer.remove(0, kHeaderSize);

  if (header.left(kBlobMagic.size()) != kBlobMagic) {
    failInternal(tr("Encrypted file header is invalid"));
    return false;
  }

  const quint8 version = static_cast<quint8>(header.at(kBlobMagic.size()));
  if (version != kBlobVersion) {
    failInternal(tr("Unsupported encrypted file version"));
    return false;
  }

  quint32 chunkSize = 0;
  memcpy(&chunkSize, header.constData() + 9, sizeof(chunkSize));
  m_chunkSize = qFromBigEndian(chunkSize);

  quint64 originalSize = 0;
  memcpy(&originalSize, header.constData() + 13, sizeof(originalSize));
  m_originalSize = qFromBigEndian(originalSize);
  m_bytesRemaining = m_originalSize;
  m_noncePrefix = header.mid(21, kNoncePrefixSize);
  m_headerParsed = true;
  return true;
}

bool BlobDownloadWorker::processBufferedData() {
  while (!m_done && m_bytesRemaining > 0) {
    const quint64 plainChunkSize = qMin<quint64>(m_chunkSize, m_bytesRemaining);
    const quint64 encryptedChunkSize = plainChunkSize + kTagSize;
    if (static_cast<quint64>(m_buffer.size()) < encryptedChunkSize) {
      return true;
    }

    const QByteArray encryptedChunk =
        m_buffer.left(static_cast<qsizetype>(encryptedChunkSize));
    m_buffer.remove(0, static_cast<qsizetype>(encryptedChunkSize));

    const QByteArray nonce = makeNonce(m_noncePrefix, m_chunkIndex++);
    QByteArray plainChunk(static_cast<qsizetype>(plainChunkSize),
                          Qt::Uninitialized);
    unsigned long long plainLength = 0;

    const int result = crypto_aead_chacha20poly1305_ietf_decrypt(
        reinterpret_cast<unsigned char*>(plainChunk.data()), &plainLength,
        nullptr,
        reinterpret_cast<const unsigned char*>(encryptedChunk.constData()),
        static_cast<unsigned long long>(encryptedChunk.size()), nullptr, 0,
        reinterpret_cast<const unsigned char*>(nonce.constData()),
        reinterpret_cast<const unsigned char*>(m_key.constData()));

    if (result != 0) {
      failInternal(tr("Failed to decrypt the downloaded file"));
      return false;
    }

    plainChunk.resize(static_cast<qsizetype>(plainLength));
    if (m_outputFile == nullptr ||
        m_outputFile->write(plainChunk) != plainChunk.size()) {
      failInternal(tr("Failed to write the downloaded file"));
      return false;
    }

    m_processedBytes += static_cast<quint64>(plainLength);
    m_bytesRemaining -= static_cast<quint64>(plainLength);

    const quint64 totalBytes =
        m_expectedFileSize > 0 ? m_expectedFileSize : m_originalSize;
    emit progressUpdated(m_processedBytes, totalBytes);
  }

  return !m_done;
}

void BlobDownloadWorker::failInternal(const QString& errorText) {
  if (m_done) {
    return;
  }

  m_done = true;
  cleanupOutputFile(false);
  emit failed(errorText);
}

void BlobDownloadWorker::cleanupOutputFile(bool commit) {
  if (m_outputFile == nullptr) {
    return;
  }

  if (!commit) {
    m_outputFile->cancelWriting();
  }

  delete m_outputFile;
  m_outputFile = nullptr;
}

namespace {

BlobPreparationResult prepareEncryptedBlob(
    const QString& sourcePath, const QString& encryptedPath,
    const BlobProgressCallback& progressCallback = {}) {
  BlobPreparationResult result;
  result.encryptedPath = encryptedPath;

  QFile sourceFile(sourcePath);
  if (!sourceFile.open(QIODevice::ReadOnly)) {
    result.errorText = QObject::tr("Failed to read the selected file");
    return result;
  }

  QFile encryptedFile(encryptedPath);
  if (!encryptedFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    result.errorText = QObject::tr("Failed to create encrypted file blob");
    return result;
  }

  QByteArray key(kKeySize, Qt::Uninitialized);
  randombytes_buf(key.data(), key.size());

  QByteArray noncePrefix(kNoncePrefixSize, Qt::Uninitialized);
  randombytes_buf(noncePrefix.data(), noncePrefix.size());

  const quint64 originalSize = static_cast<quint64>(sourceFile.size());
  quint64 processedBytes = 0;

  QByteArray header;
  header.reserve(kHeaderSize);
  header.append(kBlobMagic);
  header.append(static_cast<char>(kBlobVersion));

  quint32 beChunkSize = qToBigEndian(kFileChunkSize);
  quint64 beOriginalSize = qToBigEndian(originalSize);
  header.append(reinterpret_cast<const char*>(&beChunkSize),
                sizeof(beChunkSize));
  header.append(reinterpret_cast<const char*>(&beOriginalSize),
                sizeof(beOriginalSize));
  header.append(noncePrefix);

  if (encryptedFile.write(header) != header.size()) {
    encryptedFile.close();
    QFile::remove(encryptedPath);
    result.errorText = QObject::tr("Failed to write encrypted file header");
    return result;
  }

  quint64 chunkIndex = 0;
  while (!sourceFile.atEnd()) {
    const QByteArray plainChunk = sourceFile.read(kFileChunkSize);
    if (plainChunk.isEmpty() && sourceFile.error() != QFile::NoError) {
      encryptedFile.close();
      QFile::remove(encryptedPath);
      result.errorText = QObject::tr("Failed while reading the selected file");
      return result;
    }

    const QByteArray nonce = makeNonce(noncePrefix, chunkIndex++);
    QByteArray encryptedChunk(plainChunk.size() + kTagSize, Qt::Uninitialized);
    unsigned long long encryptedLength = 0;

    const int encryptResult = crypto_aead_chacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char*>(encryptedChunk.data()),
        &encryptedLength,
        reinterpret_cast<const unsigned char*>(plainChunk.constData()),
        static_cast<unsigned long long>(plainChunk.size()), nullptr, 0, nullptr,
        reinterpret_cast<const unsigned char*>(nonce.constData()),
        reinterpret_cast<const unsigned char*>(key.constData()));

    if (encryptResult != 0) {
      encryptedFile.close();
      QFile::remove(encryptedPath);
      result.errorText = QObject::tr("Failed to encrypt the selected file");
      return result;
    }

    encryptedChunk.resize(static_cast<qsizetype>(encryptedLength));
    if (encryptedFile.write(encryptedChunk) != encryptedChunk.size()) {
      encryptedFile.close();
      QFile::remove(encryptedPath);
      result.errorText = QObject::tr("Failed to write encrypted file blob");
      return result;
    }

    processedBytes += static_cast<quint64>(plainChunk.size());
    if (progressCallback) {
      progressCallback(processedBytes, originalSize);
    }
  }

  encryptedFile.close();

  result.fileData.fileName = QFileInfo(sourcePath).fileName();
  result.fileData.fileSize = originalSize;
  result.fileData.fileKey = QString::fromUtf8(key.toBase64(
      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
  result.fileData.localPath = sourcePath;
  result.fileData.encryption = kEncryptionName;
  result.fileData.blobSize =
      static_cast<quint64>(QFileInfo(encryptedPath).size());
  result.success = true;
  return result;
}

}  // namespace

FileTransferManager::FileTransferManager(ConnectionManager* connectionManager,
                                         DatabaseManager* databaseManager,
                                         MessageModel* messageModel,
                                         ContactsModel* contactsModel,
                                         SettingsManager* settingsManager,
                                         QObject* parent)
    : QObject(parent),
      m_connectionManager(connectionManager),
      m_db(databaseManager),
      m_messageModel(messageModel),
      m_contactsModel(contactsModel),
      m_settingsManager(settingsManager) {
  connect(m_connectionManager, &ConnectionManager::requestStateChanged, this,
          &FileTransferManager::onRequestStateChanged);
  connect(m_connectionManager, &ConnectionManager::authChanged, this,
          [this](const QString& serverId, bool success) {
            if (success) {
              retryPendingFileUploads(serverId);
              retryPendingAlbumUploads(serverId);
              retryPendingAlbumMessages(serverId);
            }
          });
  connect(m_messageModel, &MessageModel::messagesLoaded, this,
          &FileTransferManager::onChatOpened);
  connect(&MainSignals::instance(), &MainSignals::localMessageDelete, this,
          &FileTransferManager::onMessageDeleteRequested);
  connect(&MainSignals::instance(), &MainSignals::messageDelete, this,
          &FileTransferManager::onMessageDeleteRequested);
  connect(&MainSignals::instance(), &MainSignals::mediaMessageReceived, this,
          &FileTransferManager::onMediaMessageReceived);
}

bool FileTransferManager::autoDownloadEnabledFor(
    const FileMessageData& fileData) const {
  if (m_settingsManager == nullptr) {
    return false;
  }
  if (filemessage::isAudioMessage(fileData)) {
    return m_settingsManager->getBoolSetting("autoDownloadVoice", true);
  }
  if (filemessage::isImageMessage(fileData) ||
      filemessage::isVideoMessage(fileData)) {
    return m_settingsManager->getBoolSetting("autoDownloadMedia", true);
  }
  return m_settingsManager->getBoolSetting("autoDownloadFiles", false);
}

void FileTransferManager::onMediaMessageReceived(const QString& serverId,
                                                 const QString& fromPubKey,
                                                 quint64 messageId,
                                                 bool isAlbum) {
  const QByteArray content =
      m_db->getMessageContent(serverId, fromPubKey, messageId);
  if (content.isEmpty()) {
    return;
  }

  if (isAlbum) {
    AlbumMessageData album;
    if (!filemessage::deserializeAlbum(content, &album)) {
      return;
    }
    for (int i = 0; i < album.items.size(); ++i) {
      if (!filemessage::hasLocalFile(album.items[i]) &&
          autoDownloadEnabledFor(album.items[i])) {
        downloadAlbumItem(serverId, fromPubKey, messageId, i);
      }
    }
    return;
  }

  FileMessageData fileData;
  if (!filemessage::deserialize(content, &fileData)) {
    return;
  }
  if (!filemessage::hasLocalFile(fileData) && autoDownloadEnabledFor(fileData)) {
    downloadFile(serverId, fromPubKey, messageId);
  }
}

void FileTransferManager::selectAndSendFile(const QString& serverId,
                                            const QString& contactPubKey) {
  if (serverId.isEmpty() || contactPubKey.isEmpty()) {
    postInfoMessage(serverId, contactPubKey,
                    tr("Choose a chat before sending a file"));
    return;
  }

  filepicker::pickOpenFile(
      this, tr("Choose file"),
      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
      tr("All files (*)"),
      [this, serverId, contactPubKey](const QString& filePath) {
        if (filePath.isEmpty()) {
          return;
        }

        sendFile(serverId, contactPubKey, filePath);
      });
}

void FileTransferManager::selectAndSendMedia(const QString& serverId,
                                             const QString& contactPubKey) {
  if (serverId.isEmpty() || contactPubKey.isEmpty()) {
    postInfoMessage(serverId, contactPubKey,
                    tr("Choose a chat before sending media"));
    return;
  }

  filepicker::pickOpenFile(
      this, tr("Choose media"),
      QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
      tr("Media files (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.mp4 *.mkv "
         "*.webm *.avi *.mov *.m4v)") +
          QStringLiteral(";;") +
          tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp)") +
          QStringLiteral(";;") +
          tr("Videos (*.mp4 *.mkv *.webm *.avi *.mov *.m4v)"),
      [this, serverId, contactPubKey](const QString& filePath) {
        if (filePath.isEmpty()) {
          return;
        }

        const QMimeDatabase mimeDatabase;
        const QString mimeType =
            mimeDatabase.mimeTypeForFile(filePath, QMimeDatabase::MatchContent)
                .name();
        const QString mediaType = detectMediaType(filePath, mimeType);
        if (mediaType.isEmpty()) {
          postInfoMessage(
              serverId, contactPubKey,
              tr("Selected file is not a supported image or video"));
          return;
        }

        FileMessageData fileData;
        const QFileInfo sourceInfo(filePath);
        fileData.fileName = sourceInfo.fileName();
        fileData.localPath = filePath;
        fileData.mediaType = mediaType;
        fileData.mimeType = mimeType;

        sendBlobMessage(serverId, contactPubKey, filePath,
                        MessageModel::MESSAGE_TYPE::FILE, fileData);
      });
}

void FileTransferManager::sendAudioMessage(const QString& serverId,
                                           const QString& contactPubKey,
                                           const QString& filePath,
                                           quint64 durationMs,
                                           const QList<int>& waveform,
                                           const ReplyInfo& reply) {
  FileMessageData fileData;
  const QFileInfo sourceInfo(filePath);
  fileData.fileName = sourceInfo.fileName();
  fileData.localPath = filePath;
  fileData.mediaType = QStringLiteral("audio");
  fileData.mimeType = QString::fromUtf8(voiceclip::kDefaultMimeType);
  fileData.durationMs = durationMs;
  fileData.waveform = waveform;

  sendBlobMessage(serverId, contactPubKey, filePath,
                  MessageModel::MESSAGE_TYPE::AUDIO, fileData, nullptr, -1,
                  reply);
}

void FileTransferManager::downloadFile(const QString& serverId,
                                       const QString& contactPubKey,
                                       quint64 messageId) {
  const QByteArray content =
      m_db->getMessageContent(serverId, contactPubKey, messageId);

  FileMessageData fileData;
  if (!filemessage::deserialize(content, &fileData)) {
    postInfoMessage(serverId, contactPubKey, tr("File metadata is corrupted"));
    return;
  }

  if (filemessage::hasLocalFile(fileData)) {
    openLocalFile(fileData.localPath);
    return;
  }

  const QString accessToken = m_connectionManager->getAccessToken(serverId);
  const QString urlString = buildFilesUrl(serverId, fileData.fileId);
  if (accessToken.isEmpty() || urlString.isEmpty()) {
    // The file API is not ready yet -- typically an auto-download that fired
    // before the connection finished authenticating (e.g. right after the app
    // is opened). Abort quietly: posting this to the chat spams the
    // conversation, and the download runs again once the connection is up
    // (auto-download on the next sync, or when the user taps the item).
    qWarning() << "[FileTransferManager] [downloadFile] File API not ready, "
                  "skipping download"
               << "serverId=" << serverId << "messageId=" << messageId;
    return;
  }

  QByteArray key = QByteArray::fromBase64(fileData.fileKey.toUtf8(),
                                          QByteArray::Base64UrlEncoding);
  if (key.size() != kKeySize) {
    postInfoMessage(serverId, contactPubKey, tr("File key is invalid"));
    return;
  }

  auto* download = new PendingBlobDownload();
  download->serverId = serverId;
  download->contactPubKey = contactPubKey;
  download->messageId = messageId;
  download->fileData = fileData;
  download->finalPath = makeUniqueTargetPath(fileData.fileName);
  download->workerThread = new QThread();

  const bool ignoreSslErrors =
      m_settingsManager != nullptr &&
      m_settingsManager->getBoolSetting("ignoreSslErrors", false);
  download->worker =
      new BlobDownloadWorker(urlString, accessToken, ignoreSslErrors,
                             download->finalPath, key, fileData.fileSize);
  download->worker->moveToThread(download->workerThread);
  connect(download->workerThread, &QThread::finished, download->worker,
          &QObject::deleteLater);
  connect(download->workerThread, &QThread::finished, download->workerThread,
          &QObject::deleteLater);

  const QString downloadKey = transferKey(serverId, contactPubKey, messageId);
  download->downloadKey = downloadKey;
  m_activeDownloads.insert(downloadKey, download);
  setTransferProgress(serverId, contactPubKey, messageId, tr("Downloading"),
                      0.0);
  qWarning() << "[FileTransferManager] [downloadFile] Started download"
             << "messageId=" << messageId << "serverId=" << serverId
             << "fileId=" << fileData.fileId;

  connect(download->worker, &BlobDownloadWorker::progressUpdated, this,
          [this, downloadKey](quint64 processedBytes, quint64 totalBytes) {
            auto* activeDownload =
                m_activeDownloads.value(downloadKey, nullptr);
            if (activeDownload == nullptr) {
              return;
            }

            const qreal progress = totalBytes == 0
                                       ? 0.0
                                       : static_cast<qreal>(processedBytes) /
                                             static_cast<qreal>(totalBytes);

            setTransferProgress(
                activeDownload->serverId, activeDownload->contactPubKey,
                activeDownload->messageId, tr("Downloading"), progress);
          });

  connect(download->worker, &BlobDownloadWorker::failed, this,
          [this, downloadKey](const QString& errorText) {
            auto* activeDownload =
                m_activeDownloads.value(downloadKey, nullptr);
            if (activeDownload == nullptr) {
              return;
            }

            failDownload(activeDownload, errorText);
          });

  connect(download->worker, &BlobDownloadWorker::completed, this,
          [this, downloadKey]() {
            auto* activeDownload =
                m_activeDownloads.value(downloadKey, nullptr);
            if (activeDownload == nullptr) {
              return;
            }

            finishDownload(activeDownload);
          });

  connect(download->worker, &BlobDownloadWorker::canceled, this,
          [this, downloadKey]() {
            auto* activeDownload =
                m_activeDownloads.value(downloadKey, nullptr);
            if (activeDownload == nullptr) {
              return;
            }

            clearTransferState(activeDownload->serverId,
                               activeDownload->contactPubKey,
                               activeDownload->messageId);
            cleanupDownload(activeDownload);
          });

  download->workerThread->start();
  QMetaObject::invokeMethod(download->worker, "initialize",
                            Qt::QueuedConnection);
}

QString FileTransferManager::localFileUrl(const QString& localPath) const {
  if (localPath.isEmpty()) {
    return QString();
  }

  if (localPath.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
    return QUrl(localPath).toString();
  }

  return QUrl::fromLocalFile(localPath).toString();
}

void FileTransferManager::openLocalFile(const QString& localPath) {
  fileopener::openLocalFile(localPath);
}

void FileTransferManager::openContainingFolder(const QString& localPath) {
  fileopener::openContainingFolder(localPath);
}

void FileTransferManager::cancelUpload(const QString& serverId,
                                       const QString& contactPubKey,
                                       quint64 messageId) {
  auto* upload = m_activeUploads.value(
      transferKey(serverId, contactPubKey, messageId), nullptr);
  if (upload == nullptr) {
    qWarning() << "[FileTransferManager] [cancelUpload] No active upload for "
                  "messageId="
               << messageId;
    return;
  }

  qWarning() << "[FileTransferManager] [cancelUpload] Cancel requested"
             << "messageId=" << messageId << "reply=" << upload->reply
             << "watcher=" << upload->watcher;

  upload->canceled = true;

  if (upload->reply != nullptr) {
    qWarning() << "[FileTransferManager] [cancelUpload] Aborting network reply"
               << "messageId=" << messageId;
    upload->reply->abort();
    return;
  }

  removePendingBubble(upload);

  if (upload->watcher == nullptr && upload->reply == nullptr) {
    QFile::remove(upload->encryptedPath);
    releaseUpload(upload);
  }
}

void FileTransferManager::cancelDownload(const QString& serverId,
                                         const QString& contactPubKey,
                                         quint64 messageId) {
  auto* download = m_activeDownloads.value(
      transferKey(serverId, contactPubKey, messageId), nullptr);
  if (download == nullptr) {
    qWarning() << "[FileTransferManager] [cancelDownload] No active download "
                  "for messageId="
               << messageId;
    return;
  }

  qWarning() << "[FileTransferManager] [cancelDownload] Cancel requested"
             << "messageId=" << messageId << "worker=" << download->worker;

  download->canceled = true;
  if (download->worker != nullptr) {
    QMetaObject::invokeMethod(download->worker, "cancel", Qt::QueuedConnection);
  }
}

void FileTransferManager::sendFile(const QString& serverId,
                                   const QString& contactPubKey,
                                   const QString& filePath) {
  sendBlobMessage(serverId, contactPubKey, filePath,
                  MessageModel::MESSAGE_TYPE::FILE, FileMessageData());
}

void FileTransferManager::sendBlobMessage(
    const QString& serverId, const QString& contactPubKey,
    const QString& filePath, MessageModel::MESSAGE_TYPE messageType,
    const FileMessageData& initialFileData, AlbumUpload* album, int albumIndex,
    const ReplyInfo& reply) {
  QFileInfo sourceInfo(filePath);
  if (!sourceInfo.exists() || !sourceInfo.isFile()) {
    if (album != nullptr) {
      failAlbum(album, tr("Selected file does not exist"));
    } else {
      postInfoMessage(serverId, contactPubKey,
                      tr("Selected file does not exist"));
    }
    return;
  }

  // buildFilesUrl() may legitimately be empty right now (offline, or the
  // connection was briefly torn down, e.g. by the native file picker). That's
  // not fatal: the file still gets encrypted and queued below, and the
  // accessToken check after encryption falls back to storePendingFileUpload()
  // so retryPendingFileUploads() picks it up and sends it once we reconnect.
  auto* upload = new PendingUpload();
  upload->serverId = serverId;
  upload->contactPubKey = contactPubKey;
  upload->sourcePath = filePath;
  upload->uploadUrl = buildFilesUrl(serverId);
  upload->messageId = m_messageModel->uniqId();
  upload->timestamp =
      static_cast<quint64>(QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
  upload->messageType = messageType;
  upload->encryptedPath = filesDirectory() + QLatin1String("/.upload_") +
                          QString::number(QDateTime::currentMSecsSinceEpoch()) +
                          QLatin1String("_") +
                          QString::number(upload->messageId) +
                          QLatin1String(".arcblob");
  upload->fileData = initialFileData;
  if (upload->fileData.fileName.isEmpty()) {
    upload->fileData.fileName = sourceInfo.fileName();
  }
  upload->fileData.fileSize = static_cast<quint64>(sourceInfo.size());
  upload->fileData.localPath = filePath;
  upload->fileData.encryption = kEncryptionName;
  upload->previewText = filemessage::previewText(upload->fileData);
  upload->album = album;
  upload->albumIndex = albumIndex;
  upload->replyInfo = reply;

  const QString uploadKey =
      transferKey(upload->serverId, upload->contactPubKey, upload->messageId);
  upload->uploadKey = uploadKey;
  m_activeUploads.insert(uploadKey, upload);
  // Album items share a single bubble owned by the coordinator, so they don't
  // add their own; standalone uploads get an optimistic bubble here.
  if (album == nullptr) {
    if (messageType == MessageModel::MESSAGE_TYPE::AUDIO) {
      m_messageModel->addPendingAudioMessage(
          upload->messageId, upload->serverId, upload->contactPubKey,
          upload->previewText, filemessage::serialize(upload->fileData),
          upload->timestamp, upload->replyInfo);
    } else {
      m_messageModel->addPendingFileMessage(
          upload->messageId, upload->serverId, upload->contactPubKey,
          upload->previewText, filemessage::serialize(upload->fileData),
          upload->timestamp, upload->replyInfo);
    }
    setTransferState(upload->serverId, upload->contactPubKey, upload->messageId,
                     0.0, tr("Encrypting file"));
  }

  auto* watcher = new QFutureWatcher<BlobPreparationResult>(this);
  upload->watcher = watcher;

  connect(watcher, &QFutureWatcher<BlobPreparationResult>::finished, this,
          [this, upload]() {
            if (upload->watcher == nullptr) {
              return;
            }

            const BlobPreparationResult result = upload->watcher->result();
            upload->watcher->deleteLater();
            upload->watcher = nullptr;

            if (!result.success) {
              QFile::remove(result.encryptedPath);
              if (!upload->canceled) {
                failUpload(upload, result.errorText);
              } else {
                releaseUpload(upload);
              }
              return;
            }

            const QString mediaType = upload->fileData.mediaType;
            const QString mimeType = upload->fileData.mimeType;
            const quint64 durationMs = upload->fileData.durationMs;
            const QList<int> waveform = upload->fileData.waveform;
            const QString preferredName = upload->fileData.fileName;

            upload->encryptedPath = result.encryptedPath;
            upload->fileData = result.fileData;
            upload->fileData.mediaType = mediaType;
            upload->fileData.mimeType = mimeType;
            upload->fileData.durationMs = durationMs;
            upload->fileData.waveform = waveform;
            if (!preferredName.isEmpty()) {
              upload->fileData.fileName = preferredName;
            }
            if (upload->canceled) {
              QFile::remove(upload->encryptedPath);
              releaseUpload(upload);
              return;
            }

            const QString accessToken =
                m_connectionManager->getAccessToken(upload->serverId);
            if (accessToken.isEmpty()) {
              // Offline: queue the whole album instead of failing it. Keep this
              // item's encrypted blob and record it on the coordinator; once
              // every item has drained, albumItemDone() persists the album so
              // retryPendingAlbumUploads() uploads and assembles it on the next
              // reconnect. Standalone uploads fall back to the file queue below.
              if (upload->album != nullptr) {
                AlbumUpload* album = upload->album;
                album->deferredItems++;
                if (upload->albumIndex >= 0 &&
                    upload->albumIndex < album->items.size()) {
                  album->items[upload->albumIndex] = upload->fileData;
                  album->deferredPaths[upload->albumIndex] =
                      upload->encryptedPath;
                  album->itemMessageIds[upload->albumIndex] = upload->messageId;
                }
                // Do NOT remove the encrypted blob — the retry re-uploads it.
                releaseUpload(upload);
                return;
              }
              m_db->storePendingFileUpload(
                  upload->serverId, upload->contactPubKey, upload->messageId,
                  static_cast<int>(upload->messageType), upload->encryptedPath,
                  filemessage::serialize(upload->fileData), upload->timestamp,
                  upload->previewText);
              m_contactsModel->setLastMessageAt(upload->serverId,
                                               upload->contactPubKey,
                                               upload->previewText,
                                               upload->timestamp);
              m_messageModel->setFileTransferState(
                  upload->serverId, upload->contactPubKey, upload->messageId,
                  false, false, 0.0, QString());
              releaseUpload(upload);
              return;
            }

            upload->accessToken = accessToken;
            setTransferProgress(upload->serverId, upload->contactPubKey,
                                upload->messageId, tr("Uploading"), 0.0);
            startUploadRequest(upload);
          });

  watcher->setFuture(QtConcurrent::run(
      [this, upload, filePath, encryptedPath = upload->encryptedPath,
       serverId = upload->serverId, contactPubKey = upload->contactPubKey]() {
        return prepareEncryptedBlob(
            filePath, encryptedPath,
            [this, serverId, contactPubKey, messageId = upload->messageId](
                quint64 processedBytes, quint64 totalBytes) {
              const qreal progress = totalBytes == 0
                                         ? 0.0
                                         : static_cast<qreal>(processedBytes) /
                                               static_cast<qreal>(totalBytes);

              QMetaObject::invokeMethod(
                  this,
                  [this, serverId, contactPubKey, messageId, progress]() {
                    auto* currentUpload = m_activeUploads.value(
                        transferKey(serverId, contactPubKey, messageId),
                        nullptr);
                    if (currentUpload == nullptr || currentUpload->canceled ||
                        currentUpload->reply != nullptr) {
                      return;
                    }

                    setTransferProgress(serverId, contactPubKey, messageId,
                                        tr("Encrypting"), progress);
                  },
                  Qt::QueuedConnection);
            });
      }));
}

void FileTransferManager::finalizeUpload(PendingUpload* upload,
                                         const QByteArray& responseBody) {
  QFile::remove(upload->encryptedPath);
  m_db->deletePendingFileUpload(upload->messageId, upload->serverId);

  const auto json =
      nlohmann::json::parse(responseBody.constData(), nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    failUpload(upload, tr("Server returned an invalid upload response"));
    return;
  }

  upload->fileData.fileId =
      QString::fromStdString(json.value("id", std::string()));
  upload->fileData.fileSha256 =
      QString::fromStdString(json.value("sha256", std::string()));
  upload->fileData.blobSize = filemessage::readUInt64(json, "size");

  if (upload->fileData.fileId.isEmpty()) {
    failUpload(upload, tr("Server did not return a file id"));
    return;
  }

  // Album item: hand the finished metadata to the coordinator; the album wire
  // message and DB row are emitted once the last item is done.
  if (upload->album != nullptr) {
    AlbumUpload* album = upload->album;
    if (upload->albumIndex >= 0 && upload->albumIndex < album->items.size()) {
      album->items[upload->albumIndex] = upload->fileData;
      // This item is on the server now; its encrypted blob is being removed, so
      // drop the path so a re-persist won't try to re-upload a gone blob.
      if (upload->albumIndex < album->deferredPaths.size()) {
        album->deferredPaths[upload->albumIndex].clear();
      }
    }
    album->completedItems++;
    const bool done = album->completedItems >= album->totalItems;
    updateAlbumUploadProgress(album);
    // For a queued album being retried, checkpoint progress after each item so a
    // fresh disconnect resumes from here instead of losing the album or trying
    // to re-upload already-uploaded (now-deleted) blobs.
    if (album->persisted && !done) {
      persistAlbumUpload(album);
    }
    releaseUpload(upload);
    if (done && !album->failed) {
      finalizeAlbum(album);
    }
    return;
  }

  const QByteArray content = filemessage::serialize(upload->fileData);

  nlohmann::json messageJson;
  messageJson["type"] = upload->messageType == MessageModel::MESSAGE_TYPE::AUDIO
                            ? "audio"
                            : "file";
  messageJson["timestamp"] = static_cast<uint64_t>(upload->timestamp);
  messageJson["message_id"] = static_cast<uint64_t>(upload->messageId);
  messageJson["file_id"] = upload->fileData.fileId.toStdString();
  messageJson["file_name"] = upload->fileData.fileName.toStdString();
  messageJson["file_size"] = static_cast<uint64_t>(upload->fileData.fileSize);
  messageJson["file_sha256"] = upload->fileData.fileSha256.toStdString();
  messageJson["file_key"] = upload->fileData.fileKey.toStdString();
  messageJson["blob_size"] = static_cast<uint64_t>(upload->fileData.blobSize);
  messageJson["encryption"] = upload->fileData.encryption.toStdString();
  if (!upload->fileData.mediaType.isEmpty()) {
    messageJson["media_type"] = upload->fileData.mediaType.toStdString();
  }
  if (!upload->fileData.mimeType.isEmpty()) {
    messageJson["mime_type"] = upload->fileData.mimeType.toStdString();
  }
  if (upload->fileData.durationMs > 0) {
    messageJson["duration_ms"] =
        static_cast<uint64_t>(upload->fileData.durationMs);
  }
  if (!upload->fileData.waveform.isEmpty()) {
    messageJson["waveform"] = nlohmann::json::array();
    for (int value : upload->fileData.waveform) {
      messageJson["waveform"].push_back(value);
    }
  }
  if (upload->replyInfo.replyTo > 0) {
    messageJson["reply_to"] = static_cast<uint64_t>(upload->replyInfo.replyTo);
    messageJson["reply_preview"] = upload->replyInfo.preview.toStdString();
    messageJson["reply_kind"] = upload->replyInfo.kind.toStdString();
    messageJson["reply_author_is_sender"] = upload->replyInfo.isOwn;
  }

  const uint32_t requestId = m_connectionManager->e2eSendJSON(
      upload->serverId, upload->contactPubKey,
      QString::fromStdString(messageJson.dump()));

  if (requestId == 0) {
    failUpload(upload, tr("Failed to send encrypted file message"));
    return;
  }

  m_db->addTypedMessage(upload->messageId, upload->serverId,
                        upload->contactPubKey, upload->previewText,
                        upload->messageType, upload->timestamp, true, false,
                        false, false, content, upload->replyInfo);

  m_messageModel->completePendingFileMessage(
      upload->serverId, upload->contactPubKey, upload->messageId, content);

  m_contactsModel->setLastMessageAt(upload->serverId, upload->contactPubKey,
                                    upload->previewText, upload->timestamp);
  m_pendingStatuses.insert(
      requestId, PendingMessageStatus{upload->serverId, upload->contactPubKey,
                                      upload->messageId});

  releaseUpload(upload);
}

void FileTransferManager::failUpload(PendingUpload* upload,
                                     const QString& errorText) {
  // Keep a queued album's encrypted blob: failAlbum() leaves the album's
  // pending_album_uploads row in place and the next reconnect re-uploads from
  // this blob. Deleting it here would strand the album (its row still points at
  // a now-missing file) and it would be dropped on resume.
  if (!(upload->album != nullptr && upload->album->persisted)) {
    QFile::remove(upload->encryptedPath);
  }
  m_db->deletePendingFileUpload(upload->messageId, upload->serverId);
  if (upload->album != nullptr) {
    failAlbum(upload->album, errorText);
    releaseUpload(upload);
    return;
  }
  removePendingBubble(upload);
  if (!errorText.isEmpty()) {
    postInfoMessage(upload->serverId, upload->contactPubKey, errorText);
  }
  releaseUpload(upload);
}

void FileTransferManager::removePendingBubble(PendingUpload* upload) {
  if (upload == nullptr || upload->messageId == 0) {
    return;
  }

  m_messageModel->removeTransientMessage(
      upload->serverId, upload->contactPubKey, upload->messageId);
}

void FileTransferManager::releaseUpload(PendingUpload* upload) {
  if (upload == nullptr) {
    return;
  }

  m_activeUploads.remove(
      transferKey(upload->serverId, upload->contactPubKey, upload->messageId));

  if (upload->reply != nullptr || upload->watcher != nullptr) {
    return;
  }

  AlbumUpload* album = upload->album;
  delete upload;
  if (album != nullptr) {
    albumItemDone(album);
  }
}

void FileTransferManager::albumItemDone(AlbumUpload* album) {
  if (album == nullptr) {
    return;
  }
  album->outstanding--;
  if (album->outstanding > 0) {
    return;
  }

  // Every item upload for this album has now drained.
  if (album->failed) {
    // A failed *queued* album keeps its pending_album_uploads row and bubble so
    // the next reconnect resumes it (failAlbum handled the UI); only free the
    // in-memory coordinator here. A failed fresh album is simply reclaimed.
    m_activeAlbums.remove(album->messageId);
    delete album;
    return;
  }

  if (album->deferredItems > 0) {
    // At least one item couldn't be uploaded (offline). Persist the whole album
    // so retryPendingAlbumUploads() finishes it on the next reconnect, and park
    // the bubble as waiting.
    persistAlbumUpload(album);
    m_messageModel->setFileTransferState(
        album->serverId, album->contactPubKey, album->messageId, false, false,
        0.0, tr("Waiting for connection"));
    m_activeAlbums.remove(album->messageId);
    delete album;
    return;
  }

  // All items uploaded: finalizeAlbum() (invoked from finalizeUpload once
  // completedItems == totalItems) owns the coordinator, so nothing to do here.
}

QVariantMap FileTransferManager::classifyFile(const QString& filePath) const {
  QVariantMap map;
  const QFileInfo info(filePath);
  const QMimeDatabase mimeDatabase;
  const QString mimeType =
      mimeDatabase.mimeTypeForFile(filePath, QMimeDatabase::MatchContent).name();
  QString mediaType = detectMediaType(filePath, mimeType);  // image/video/""
  const bool isAudio = mimeType.startsWith(QStringLiteral("audio/"));
  if (mediaType.isEmpty() && isAudio) {
    mediaType = QStringLiteral("audio");
  }
  const bool isImage = (mediaType == QStringLiteral("image"));
  const bool isVideo = (mediaType == QStringLiteral("video"));
  map["path"] = filePath;
  map["name"] = info.fileName();
  map["size"] = QVariant::fromValue<quint64>(static_cast<quint64>(info.size()));
  map["mediaType"] = mediaType;
  map["mimeType"] = mimeType;
  map["isImage"] = isImage;
  map["isVideo"] = isVideo;
  map["isAudio"] = isAudio;
  map["isMedia"] = isImage || isVideo;
  return map;
}

void FileTransferManager::pickFilesForStaging(const QString& serverId,
                                              const QString& contactPubKey,
                                              bool mediaOnly) {
  if (serverId.isEmpty() || contactPubKey.isEmpty()) {
    postInfoMessage(serverId, contactPubKey,
                    tr("Choose a chat before sending files"));
    return;
  }

  const QString title = mediaOnly ? tr("Choose media") : tr("Choose files");
  const QString dir = QStandardPaths::writableLocation(
      mediaOnly ? QStandardPaths::PicturesLocation
                : QStandardPaths::DocumentsLocation);
  const QString filter =
      mediaOnly ? tr("Media files (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.mp4 "
                     "*.mkv *.webm *.avi *.mov *.m4v)")
                : tr("All files (*)");

  filepicker::pickOpenFiles(
      this, title, dir, filter,
      [this, serverId, contactPubKey, mediaOnly](const QStringList& paths) {
        if (paths.isEmpty()) {
          return;
        }
        QVariantList items;
        for (const QString& path : paths) {
          if (path.isEmpty()) {
            continue;
          }
          QVariantMap item = classifyFile(path);
          if (mediaOnly && !item.value("isMedia").toBool()) {
            continue;  // drop non-media when the media picker was used
          }
          items.append(item);
        }
        if (!items.isEmpty()) {
          emit filesPicked(serverId, contactPubKey, items);
        }
      });
}

void FileTransferManager::sendAlbum(const QString& serverId,
                                    const QString& contactPubKey,
                                    const QStringList& filePaths,
                                    const QString& caption, quint64 replyTo,
                                    const QString& replyPreview,
                                    const QString& replyKind, bool replyIsOwn) {
  QStringList validPaths;
  for (const QString& path : filePaths) {
    const QFileInfo info(path);
    if (info.exists() && info.isFile()) {
      validPaths.append(path);
    }
  }
  if (validPaths.isEmpty()) {
    postInfoMessage(serverId, contactPubKey, tr("No files to send"));
    return;
  }

  auto* album = new AlbumUpload();
  album->serverId = serverId;
  album->contactPubKey = contactPubKey;
  album->messageId = m_messageModel->uniqId();
  album->timestamp =
      static_cast<quint64>(QDateTime::currentDateTimeUtc().toSecsSinceEpoch());
  album->caption = caption;
  album->reply = ReplyInfo{replyTo, replyPreview, replyKind, replyIsOwn};
  album->totalItems = validPaths.size();
  album->outstanding = validPaths.size();
  album->items.resize(validPaths.size());
  album->deferredPaths.resize(validPaths.size());
  album->itemMessageIds.resize(validPaths.size());

  // Optimistic bubble: preview the local originals right away.
  AlbumMessageData previewAlbum;
  previewAlbum.caption = caption;
  for (const QString& path : validPaths) {
    const QVariantMap info = classifyFile(path);
    FileMessageData item;
    item.fileName = info.value("name").toString();
    item.localPath = path;
    item.fileSize = info.value("size").toULongLong();
    item.mediaType = info.value("mediaType").toString();
    item.mimeType = info.value("mimeType").toString();
    previewAlbum.items.append(item);
  }

  m_activeAlbums.insert(album->messageId, album);
  m_messageModel->addPendingAlbumMessage(
      album->messageId, serverId, contactPubKey, caption,
      filemessage::serializeAlbum(previewAlbum), album->timestamp,
      album->reply);
  m_messageModel->setFileTransferState(
      serverId, contactPubKey, album->messageId, true, false, 0.0,
      tr("Uploading 0/%1").arg(album->totalItems));

  for (int i = 0; i < validPaths.size(); ++i) {
    const QVariantMap info = classifyFile(validPaths[i]);
    FileMessageData fileData;
    fileData.fileName = info.value("name").toString();
    fileData.localPath = validPaths[i];
    fileData.mediaType = info.value("mediaType").toString();
    fileData.mimeType = info.value("mimeType").toString();
    sendBlobMessage(serverId, contactPubKey, validPaths[i],
                    MessageModel::MESSAGE_TYPE::FILE, fileData, album, i);
  }
}

void FileTransferManager::updateAlbumUploadProgress(AlbumUpload* album) {
  if (album == nullptr) {
    return;
  }
  const qreal progress =
      album->totalItems > 0 ? static_cast<qreal>(album->completedItems) /
                                  static_cast<qreal>(album->totalItems)
                            : 0.0;
  m_messageModel->setFileTransferState(
      album->serverId, album->contactPubKey, album->messageId, true, false,
      progress,
      tr("Uploading %1/%2").arg(album->completedItems).arg(album->totalItems));
}

namespace {
// Builds the outgoing "album" wire message from already-uploaded item
// metadata. Shared by finalizeAlbum (fresh send) and
// retryPendingAlbumMessages (resend from the locally stored content after a
// failed/interrupted send), so both stay in sync.
nlohmann::json buildAlbumMessageJson(const AlbumMessageData& albumData,
                                     quint64 messageId, quint64 timestamp,
                                     const ReplyInfo& reply) {
  nlohmann::json messageJson;
  messageJson["type"] = "album";
  messageJson["timestamp"] = static_cast<uint64_t>(timestamp);
  messageJson["message_id"] = static_cast<uint64_t>(messageId);
  if (!albumData.caption.isEmpty()) {
    messageJson["caption"] = albumData.caption.toStdString();
  }
  messageJson["items"] = nlohmann::json::array();
  for (const FileMessageData& item : albumData.items) {
    // Build per-item JSON by hand so the local path never leaves the device.
    nlohmann::json itemJson;
    itemJson["file_id"] = item.fileId.toStdString();
    itemJson["file_name"] = item.fileName.toStdString();
    itemJson["file_size"] = static_cast<uint64_t>(item.fileSize);
    itemJson["file_sha256"] = item.fileSha256.toStdString();
    itemJson["file_key"] = item.fileKey.toStdString();
    itemJson["blob_size"] = static_cast<uint64_t>(item.blobSize);
    itemJson["encryption"] = item.encryption.toStdString();
    if (!item.mediaType.isEmpty()) {
      itemJson["media_type"] = item.mediaType.toStdString();
    }
    if (!item.mimeType.isEmpty()) {
      itemJson["mime_type"] = item.mimeType.toStdString();
    }
    if (item.durationMs > 0) {
      itemJson["duration_ms"] = static_cast<uint64_t>(item.durationMs);
    }
    messageJson["items"].push_back(itemJson);
  }
  if (reply.replyTo > 0) {
    messageJson["reply_to"] = static_cast<uint64_t>(reply.replyTo);
    messageJson["reply_preview"] = reply.preview.toStdString();
    messageJson["reply_kind"] = reply.kind.toStdString();
    messageJson["reply_author_is_sender"] = reply.isOwn;
  }
  return messageJson;
}
}  // namespace

void FileTransferManager::finalizeAlbum(AlbumUpload* album) {
  if (album == nullptr) {
    return;
  }

  AlbumMessageData albumData;
  albumData.caption = album->caption;
  albumData.items = album->items;
  const QByteArray content = filemessage::serializeAlbum(albumData);
  const nlohmann::json messageJson = buildAlbumMessageJson(
      albumData, album->messageId, album->timestamp, album->reply);

  const uint32_t requestId = m_connectionManager->e2eSendJSON(
      album->serverId, album->contactPubKey,
      QString::fromStdString(messageJson.dump()));

  // Every item has already finished uploading to the server by this point,
  // so on failure we must not lose the album the way failAlbum() would (no
  // DB row at all, orphaning the just-uploaded blobs with no retry). Persist
  // it exactly like the success path below either way; the only difference is
  // whether we have a requestId to track delivery with. The existing
  // isSended=0 pending-message convention (see retryPendingAlbumMessages)
  // picks unsent/undelivered rows back up on the next reconnect, same as
  // pending text messages already do.
  const QString previewText =
      album->caption.isEmpty() ? tr("Album") : album->caption;
  m_db->addTypedMessage(album->messageId, album->serverId, album->contactPubKey,
                        previewText, MessageModel::MESSAGE_TYPE::ALBUM,
                        album->timestamp, true, false, false, false, content,
                        album->reply);
  m_messageModel->completePendingAlbumMessage(
      album->serverId, album->contactPubKey, album->messageId, content);
  m_contactsModel->setLastMessageAt(album->serverId, album->contactPubKey,
                                    previewText, album->timestamp);

  if (requestId != 0) {
    m_pendingStatuses.insert(
        requestId, PendingMessageStatus{album->serverId, album->contactPubKey,
                                        album->messageId});
  }

  // The album is now a normal (isSended=0/1) message; if it was queued offline,
  // drop its pending-album-upload row. Any undelivered wire message is picked up
  // by retryPendingAlbumMessages, which never re-uploads the (already stored)
  // blobs.
  m_db->deletePendingAlbumUpload(album->messageId, album->serverId);

  m_activeAlbums.remove(album->messageId);
  delete album;
}

void FileTransferManager::failAlbum(AlbumUpload* album,
                                    const QString& errorText) {
  if (album == nullptr || album->failed) {
    return;
  }
  album->failed = true;

  // A queued (offline) album that fails mid-retry must not be lost: its
  // pending_album_uploads row (checkpointed per item) stays, and the bubble goes
  // back to "waiting" so the next reconnect resumes it — no error toast, no
  // bubble teardown. A fresh, never-queued album keeps the old fail behaviour.
  const bool wasQueued = album->persisted;

  // Cancel sibling item uploads still in flight; each drains through
  // releaseUpload -> albumItemDone, which frees the coordinator at the end.
  const QList<PendingUpload*> uploads = m_activeUploads.values();
  for (PendingUpload* upload : uploads) {
    if (upload->album == album && !upload->canceled) {
      upload->canceled = true;
      if (upload->reply != nullptr) {
        upload->reply->abort();
      }
    }
  }

  if (wasQueued) {
    m_messageModel->setFileTransferState(
        album->serverId, album->contactPubKey, album->messageId, false, false,
        0.0, tr("Waiting for connection"));
    // The DB row is intentionally kept; retryPendingAlbumUploads resumes it.
    // Deletion of the in-memory coordinator happens in albumItemDone.
    return;
  }

  m_messageModel->removeTransientMessage(album->serverId, album->contactPubKey,
                                         album->messageId);
  if (!errorText.isEmpty()) {
    postInfoMessage(album->serverId, album->contactPubKey, errorText);
  }
  // Deletion happens in albumItemDone once every item upload has been released.
}

void FileTransferManager::persistAlbumUpload(AlbumUpload* album) {
  if (album == nullptr) {
    return;
  }
  album->persisted = true;

  nlohmann::json itemsJson = nlohmann::json::array();
  for (int i = 0; i < album->items.size(); ++i) {
    nlohmann::json itemJson;
    itemJson["index"] = i;
    itemJson["message_id"] = static_cast<uint64_t>(
        i < album->itemMessageIds.size() ? album->itemMessageIds[i] : 0);
    const QString encPath =
        i < album->deferredPaths.size() ? album->deferredPaths[i] : QString();
    itemJson["encrypted_path"] = encPath.toStdString();
    // Serialized FileMessageData: carries the fileId once uploaded, otherwise
    // the encryption metadata (fileKey/sha/blobSize) needed to upload later.
    itemJson["file_data"] =
        QString::fromUtf8(filemessage::serialize(album->items[i])).toStdString();
    itemsJson.push_back(itemJson);
  }

  m_db->storePendingAlbumUpload(
      album->messageId, album->serverId, album->contactPubKey, album->caption,
      album->timestamp, album->reply, album->totalItems,
      QByteArray::fromStdString(itemsJson.dump()));
}

void FileTransferManager::retryPendingAlbumUploads(const QString& serverId) {
  const QString accessToken = m_connectionManager->getAccessToken(serverId);
  const QString urlString = buildFilesUrl(serverId);
  if (accessToken.isEmpty() || urlString.isEmpty()) {
    return;
  }

  const QVariantList rows = m_db->loadPendingAlbumUploads(serverId);
  for (const QVariant& rowVar : rows) {
    const QVariantMap row = rowVar.toMap();
    const quint64 albumMessageId =
        row[QStringLiteral("album_message_id")].toULongLong();

    // Already being (re)sent — don't drive it twice on rapid reconnects.
    if (m_activeAlbums.contains(albumMessageId)) {
      continue;
    }

    const QString contactPubKey =
        row[QStringLiteral("contact_pub_key")].toString();
    const QString caption = row[QStringLiteral("caption")].toString();
    const quint64 timestamp = row[QStringLiteral("timestamp")].toULongLong();
    const int totalItems = row[QStringLiteral("total_items")].toInt();
    const ReplyInfo reply{row[QStringLiteral("reply_to")].toULongLong(),
                          row[QStringLiteral("reply_preview")].toString(),
                          row[QStringLiteral("reply_kind")].toString(),
                          row[QStringLiteral("reply_is_own")].toBool()};
    const QByteArray itemsBlob = row[QStringLiteral("items_json")].toByteArray();

    const auto itemsJson =
        nlohmann::json::parse(itemsBlob.constData(), nullptr, false);
    if (itemsJson.is_discarded() || !itemsJson.is_array() || totalItems <= 0) {
      m_db->deletePendingAlbumUpload(albumMessageId, serverId);
      m_messageModel->removeTransientMessage(serverId, contactPubKey,
                                             albumMessageId);
      continue;
    }

    // Rebuild the coordinator and decode every item slot.
    auto* album = new AlbumUpload();
    album->serverId = serverId;
    album->contactPubKey = contactPubKey;
    album->messageId = albumMessageId;
    album->timestamp = timestamp;
    album->caption = caption;
    album->reply = reply;
    album->totalItems = totalItems;
    album->persisted = true;
    album->items.resize(totalItems);
    album->deferredPaths.resize(totalItems);
    album->itemMessageIds.resize(totalItems);

    bool valid = true;
    int needing = 0;
    for (const auto& itemJson : itemsJson) {
      if (!itemJson.is_object()) {
        valid = false;
        break;
      }
      const int index = itemJson.value("index", -1);
      if (index < 0 || index >= totalItems) {
        valid = false;
        break;
      }
      FileMessageData fileData;
      const std::string fileDataStr = itemJson.value("file_data", std::string());
      if (!filemessage::deserialize(QByteArray::fromStdString(fileDataStr),
                                    &fileData)) {
        valid = false;
        break;
      }
      const QString encPath = QString::fromStdString(
          itemJson.value("encrypted_path", std::string()));
      album->items[index] = fileData;
      album->deferredPaths[index] = encPath;
      album->itemMessageIds[index] = static_cast<quint64>(
          itemJson.value("message_id", static_cast<uint64_t>(0)));

      // An item still needs uploading when it has no server file id yet. Its
      // encrypted blob must still exist on disk, else the album is unrecoverable.
      if (fileData.fileId.isEmpty()) {
        if (encPath.isEmpty() || !QFile::exists(encPath)) {
          valid = false;
          break;
        }
        needing++;
      }
    }

    if (!valid) {
      delete album;
      m_db->deletePendingAlbumUpload(albumMessageId, serverId);
      // The queued blobs are gone (e.g. cache cleared) — drop the stuck bubble
      // rather than leave it spinning forever.
      m_messageModel->removeTransientMessage(serverId, contactPubKey,
                                             albumMessageId);
      continue;
    }

    album->completedItems = totalItems - needing;
    album->outstanding = needing;

    m_activeAlbums.insert(albumMessageId, album);

    // Re-add the optimistic bubble if it was lost (e.g. app restart).
    if (!m_messageModel->hasMessage(albumMessageId)) {
      AlbumMessageData previewAlbum;
      previewAlbum.caption = caption;
      for (const FileMessageData& item : album->items) {
        previewAlbum.items.append(item);
      }
      m_messageModel->addPendingAlbumMessage(
          albumMessageId, serverId, contactPubKey, caption,
          filemessage::serializeAlbum(previewAlbum), timestamp, reply);
    }
    m_messageModel->setFileTransferState(
        serverId, contactPubKey, albumMessageId, true, false,
        totalItems > 0 ? static_cast<qreal>(album->completedItems) /
                             static_cast<qreal>(totalItems)
                       : 0.0,
        tr("Uploading %1/%2").arg(album->completedItems).arg(totalItems));

    // Every item was already uploaded before (only the wire message was
    // missing): assemble straight away.
    if (needing == 0) {
      finalizeAlbum(album);
      continue;
    }

    // Drive the remaining item uploads from their stored encrypted blobs.
    for (int index = 0; index < totalItems; ++index) {
      if (!album->items[index].fileId.isEmpty()) {
        continue;  // already on the server
      }
      auto* upload = new PendingUpload();
      upload->serverId = serverId;
      upload->contactPubKey = contactPubKey;
      upload->messageId = album->itemMessageIds[index] != 0
                              ? album->itemMessageIds[index]
                              : m_messageModel->uniqId();
      album->itemMessageIds[index] = upload->messageId;
      upload->messageType = MessageModel::MESSAGE_TYPE::FILE;
      upload->encryptedPath = album->deferredPaths[index];
      upload->fileData = album->items[index];
      upload->timestamp = timestamp;
      upload->uploadUrl = urlString;
      upload->accessToken = accessToken;
      upload->album = album;
      upload->albumIndex = index;
      const QString key = transferKey(serverId, contactPubKey, upload->messageId);
      upload->uploadKey = key;
      m_activeUploads.insert(key, upload);
      startUploadRequest(upload);
    }
  }
}

void FileTransferManager::downloadAlbumItem(const QString& serverId,
                                            const QString& contactPubKey,
                                            quint64 messageId, int itemIndex) {
  const QByteArray content =
      m_db->getMessageContent(serverId, contactPubKey, messageId);
  AlbumMessageData album;
  if (!filemessage::deserializeAlbum(content, &album)) {
    postInfoMessage(serverId, contactPubKey, tr("Album metadata is corrupted"));
    return;
  }
  if (itemIndex < 0 || itemIndex >= album.items.size()) {
    return;
  }

  const FileMessageData& fileData = album.items[itemIndex];
  if (filemessage::hasLocalFile(fileData)) {
    openLocalFile(fileData.localPath);
    return;
  }

  const QString albumItemKey =
      transferKey(serverId, contactPubKey, messageId) + QStringLiteral("#") +
      QString::number(itemIndex);
  if (m_activeDownloads.contains(albumItemKey)) {
    return;  // already downloading this item
  }

  const QString accessToken = m_connectionManager->getAccessToken(serverId);
  const QString urlString = buildFilesUrl(serverId, fileData.fileId);
  if (accessToken.isEmpty() || urlString.isEmpty()) {
    // Not ready yet (auto-download racing the connection); abort quietly rather
    // than spamming the chat -- it retries once the file API is available.
    qWarning() << "[FileTransferManager] [downloadAlbumItem] File API not "
                  "ready, skipping download"
               << "serverId=" << serverId << "messageId=" << messageId
               << "itemIndex=" << itemIndex;
    return;
  }

  QByteArray key = QByteArray::fromBase64(fileData.fileKey.toUtf8(),
                                          QByteArray::Base64UrlEncoding);
  if (key.size() != kKeySize) {
    postInfoMessage(serverId, contactPubKey, tr("File key is invalid"));
    return;
  }

  auto* download = new PendingBlobDownload();
  download->serverId = serverId;
  download->contactPubKey = contactPubKey;
  download->messageId = messageId;
  download->albumIndex = itemIndex;
  download->downloadKey = albumItemKey;
  download->fileData = fileData;
  download->finalPath = makeUniqueTargetPath(fileData.fileName);
  download->workerThread = new QThread();

  const bool ignoreSslErrors =
      m_settingsManager != nullptr &&
      m_settingsManager->getBoolSetting("ignoreSslErrors", false);
  download->worker =
      new BlobDownloadWorker(urlString, accessToken, ignoreSslErrors,
                             download->finalPath, key, fileData.fileSize);
  download->worker->moveToThread(download->workerThread);
  connect(download->workerThread, &QThread::finished, download->worker,
          &QObject::deleteLater);
  connect(download->workerThread, &QThread::finished, download->workerThread,
          &QObject::deleteLater);

  m_activeDownloads.insert(albumItemKey, download);
  m_messageModel->setAlbumItemDownloading(serverId, contactPubKey, messageId,
                                          itemIndex, true, 0.0);

  connect(download->worker, &BlobDownloadWorker::progressUpdated, this,
          [this, albumItemKey](quint64 processedBytes, quint64 totalBytes) {
            auto* activeDownload = m_activeDownloads.value(albumItemKey, nullptr);
            if (activeDownload == nullptr) {
              return;
            }
            const qreal progress = totalBytes == 0
                                       ? 0.0
                                       : static_cast<qreal>(processedBytes) /
                                             static_cast<qreal>(totalBytes);
            m_messageModel->setAlbumItemDownloading(
                activeDownload->serverId, activeDownload->contactPubKey,
                activeDownload->messageId, activeDownload->albumIndex, true,
                progress);
          });

  connect(download->worker, &BlobDownloadWorker::failed, this,
          [this, albumItemKey](const QString& errorText) {
            auto* activeDownload = m_activeDownloads.value(albumItemKey, nullptr);
            if (activeDownload == nullptr) {
              return;
            }
            failDownload(activeDownload, errorText);
          });

  connect(download->worker, &BlobDownloadWorker::completed, this,
          [this, albumItemKey]() {
            auto* activeDownload = m_activeDownloads.value(albumItemKey, nullptr);
            if (activeDownload == nullptr) {
              return;
            }
            finishDownload(activeDownload);
          });

  connect(download->worker, &BlobDownloadWorker::canceled, this,
          [this, albumItemKey]() {
            auto* activeDownload = m_activeDownloads.value(albumItemKey, nullptr);
            if (activeDownload == nullptr) {
              return;
            }
            m_messageModel->setAlbumItemDownloading(
                activeDownload->serverId, activeDownload->contactPubKey,
                activeDownload->messageId, activeDownload->albumIndex, false,
                0.0);
            cleanupDownload(activeDownload);
          });

  download->workerThread->start();
  QMetaObject::invokeMethod(download->worker, "initialize",
                            Qt::QueuedConnection);
}

void FileTransferManager::startUploadRequest(PendingUpload* upload) {
  QFile* device = new QFile(upload->encryptedPath);
  if (!device->open(QIODevice::ReadOnly)) {
    delete device;
    failUpload(upload, tr("Failed to open encrypted file blob"));
    return;
  }

  QNetworkRequest request{QUrl(upload->uploadUrl)};
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/octet-stream"));
  request.setRawHeader("Authorization",
                       "Bearer " + upload->accessToken.toUtf8());
  request.setRawHeader("X-Core-Recipients", upload->contactPubKey.toUtf8());
  request.setHeader(QNetworkRequest::ContentLengthHeader, device->size());
  applySslPolicy(request);

  QNetworkReply* reply = m_networkManager.post(request, device);
  device->setParent(reply);
  upload->reply = reply;
  m_uploads.insert(reply, upload);
  applySslPolicy(reply);

  const QString uploadKey =
      transferKey(upload->serverId, upload->contactPubKey, upload->messageId);
  connect(reply, &QNetworkReply::uploadProgress, this,
          [this, uploadKey, uploadMessageId = upload->messageId](
              qint64 bytesSent, qint64 bytesTotal) {
            auto* currentUpload = m_activeUploads.value(uploadKey, nullptr);
            if (currentUpload == nullptr || currentUpload->canceled ||
                bytesTotal <= 0) {
              return;
            }

            const qreal progress =
                static_cast<qreal>(bytesSent) / static_cast<qreal>(bytesTotal);
            setTransferProgress(currentUpload->serverId,
                                currentUpload->contactPubKey, uploadMessageId,
                                tr("Uploading"), progress);
          });

  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    auto* upload = m_uploads.take(reply);
    if (upload == nullptr) {
      qWarning() << "[FileTransferManager] [upload finished] Reply finished "
                    "after upload was untracked"
                 << "reply=" << reply << "error=" << reply->error();
      reply->deleteLater();
      return;
    }

    upload->reply = nullptr;
    qWarning() << "[FileTransferManager] [upload finished] Entered"
               << "messageId=" << upload->messageId
               << "canceled=" << upload->canceled
               << "replyError=" << reply->error()
               << "isOpen=" << reply->isOpen();

    if (upload->canceled &&
        reply->error() == QNetworkReply::OperationCanceledError) {
      qWarning() << "[FileTransferManager] [upload finished] Handling canceled "
                    "upload cleanup"
                 << "messageId=" << upload->messageId;
      // Preserve a queued album's blob (see failUpload): a sibling item aborted
      // by failAlbum during a retry must survive for the next reconnect.
      if (!(upload->album != nullptr && upload->album->persisted)) {
        QFile::remove(upload->encryptedPath);
      }
      m_db->deletePendingFileUpload(upload->messageId, upload->serverId);
      removePendingBubble(upload);
      releaseUpload(upload);
      reply->deleteLater();
      return;
    }

    if (reply->error() != QNetworkReply::NoError) {
      failUpload(upload, networkErrorText(reply, /*isUpload=*/true));
      reply->deleteLater();
      return;
    }

    const QByteArray responseBody = reply->readAll();
    finalizeUpload(upload, responseBody);
    reply->deleteLater();
  });
}

void FileTransferManager::onRequestStateChanged(uint32_t requestId,
                                                const QString& status) {
  if (!m_pendingStatuses.contains(requestId)) {
    return;
  }

  const PendingMessageStatus pending = m_pendingStatuses.value(requestId);

  if (status == QLatin1String("received")) {
    m_messageModel->updateMessageSentStatus(
        pending.serverId, pending.contactPubKey, pending.messageId);
    m_db->updateMessageSentStatus(pending.serverId, pending.contactPubKey,
                                  pending.messageId);
    // isSended is now 1, so retryPendingAlbumMessages' query no longer
    // matches this row; safe to stop guarding it against a duplicate resend.
    m_pendingRetryAlbumMessageIds.remove(pending.messageId);
    return;
  }

  if (status == QLatin1String("delivered")) {
    m_messageModel->updateMessageSentStatus(
        pending.serverId, pending.contactPubKey, pending.messageId);
    m_messageModel->updateMessageDeliveredStatus(
        pending.serverId, pending.contactPubKey, pending.messageId);
    m_db->updateMessageSentStatus(pending.serverId, pending.contactPubKey,
                                  pending.messageId);
    m_db->updateMessageDeliveredStatus(pending.serverId, pending.contactPubKey,
                                       pending.messageId);
    m_pendingRetryAlbumMessageIds.remove(pending.messageId);
    m_pendingStatuses.remove(requestId);
  }
}

void FileTransferManager::retryPendingFileUploads(const QString& serverId) {
  const QString accessToken = m_connectionManager->getAccessToken(serverId);
  const QString urlString = buildFilesUrl(serverId);
  if (accessToken.isEmpty() || urlString.isEmpty()) {
    return;
  }

  const QVariantList rows = m_db->loadPendingFileUploads(serverId);
  for (const QVariant& rowVar : rows) {
    const QVariantMap row = rowVar.toMap();

    const quint64 messageId = row[QStringLiteral("message_id")].toULongLong();
    const QString contactPubKey =
        row[QStringLiteral("contact_pub_key")].toString();
    const QString encryptedPath =
        row[QStringLiteral("encrypted_path")].toString();
    const int messageType = row[QStringLiteral("message_type")].toInt();
    const quint64 timestamp = row[QStringLiteral("timestamp")].toULongLong();
    const QString previewText = row[QStringLiteral("preview_text")].toString();
    const QByteArray fileDataBlob =
        row[QStringLiteral("file_data")].toByteArray();

    const QString key = transferKey(serverId, contactPubKey, messageId);
    if (m_activeUploads.contains(key)) {
      continue;
    }

    if (!QFile::exists(encryptedPath)) {
      m_db->deletePendingFileUpload(messageId, serverId);
      continue;
    }

    FileMessageData fileData;
    if (!filemessage::deserialize(fileDataBlob, &fileData)) {
      m_db->deletePendingFileUpload(messageId, serverId);
      continue;
    }

    auto* upload = new PendingUpload();
    upload->serverId = serverId;
    upload->contactPubKey = contactPubKey;
    upload->messageId = messageId;
    upload->messageType =
        static_cast<MessageModel::MESSAGE_TYPE>(messageType);
    upload->encryptedPath = encryptedPath;
    upload->fileData = fileData;
    upload->timestamp = timestamp;
    upload->previewText = previewText;
    upload->uploadUrl = urlString;
    upload->accessToken = accessToken;

    m_activeUploads.insert(key, upload);

    if (!m_messageModel->hasMessage(upload->messageId)) {
      if (upload->messageType == MessageModel::MESSAGE_TYPE::AUDIO) {
        m_messageModel->addPendingAudioMessage(
            upload->messageId, upload->serverId, upload->contactPubKey,
            upload->previewText, filemessage::serialize(upload->fileData),
            upload->timestamp);
      } else {
        m_messageModel->addPendingFileMessage(
            upload->messageId, upload->serverId, upload->contactPubKey,
            upload->previewText, filemessage::serialize(upload->fileData),
            upload->timestamp);
      }
    }
    setTransferProgress(upload->serverId, upload->contactPubKey,
                        upload->messageId, tr("Uploading"), 0.0);

    startUploadRequest(upload);
  }
}

void FileTransferManager::retryPendingAlbumMessages(const QString& serverId) {
  const QVariantList rows = m_db->loadPendingAlbumMessages(serverId);
  for (const QVariant& rowVar : rows) {
    const QVariantMap row = rowVar.toMap();

    const quint64 messageId = row[QStringLiteral("id")].toULongLong();
    const QString contactPubKey =
        row[QStringLiteral("fromPubKey")].toString();
    const QByteArray contentBlob = row[QStringLiteral("content")].toByteArray();
    const quint64 timestamp = row[QStringLiteral("t")].toULongLong();

    if (messageId == 0 || contactPubKey.isEmpty() ||
        m_pendingRetryAlbumMessageIds.contains(messageId)) {
      continue;
    }

    AlbumMessageData albumData;
    if (!filemessage::deserializeAlbum(contentBlob, &albumData)) {
      continue;
    }

    ReplyInfo reply;
    reply.replyTo = row[QStringLiteral("replyTo")].toULongLong();
    reply.preview = row[QStringLiteral("replyPreview")].toString();
    reply.kind = row[QStringLiteral("replyKind")].toString();
    reply.isOwn = row[QStringLiteral("replyIsOwn")].toBool();

    const nlohmann::json messageJson =
        buildAlbumMessageJson(albumData, messageId, timestamp, reply);
    const uint32_t requestId = m_connectionManager->e2eSendJSON(
        serverId, contactPubKey, QString::fromStdString(messageJson.dump()));
    if (requestId == 0) {
      continue;
    }

    m_pendingRetryAlbumMessageIds.insert(messageId);
    m_pendingStatuses.insert(
        requestId, PendingMessageStatus{serverId, contactPubKey, messageId});
  }
}

void FileTransferManager::onChatOpened(const QString& serverId,
                                       const QString& contactPubKey) {
  if (serverId.isEmpty() || contactPubKey.isEmpty()) {
    return;
  }

  const QString accessToken = m_connectionManager->getAccessToken(serverId);
  const QString urlString = buildFilesUrl(serverId);
  const bool canUpload = !accessToken.isEmpty() && !urlString.isEmpty();

  const QVariantList rows =
      m_db->loadPendingFileUploadsForContact(serverId, contactPubKey);
  for (const QVariant& rowVar : rows) {
    const QVariantMap row = rowVar.toMap();

    const quint64 messageId = row[QStringLiteral("message_id")].toULongLong();
    const QString encryptedPath =
        row[QStringLiteral("encrypted_path")].toString();
    const int messageType = row[QStringLiteral("message_type")].toInt();
    const quint64 timestamp = row[QStringLiteral("timestamp")].toULongLong();
    const QString previewText = row[QStringLiteral("preview_text")].toString();
    const QByteArray fileDataBlob =
        row[QStringLiteral("file_data")].toByteArray();

    if (!QFile::exists(encryptedPath)) {
      m_db->deletePendingFileUpload(messageId, serverId);
      continue;
    }

    FileMessageData fileData;
    if (!filemessage::deserialize(fileDataBlob, &fileData)) {
      m_db->deletePendingFileUpload(messageId, serverId);
      continue;
    }

    const auto msgType = static_cast<MessageModel::MESSAGE_TYPE>(messageType);
    const QString key = transferKey(serverId, contactPubKey, messageId);
    const bool alreadyRunning = m_activeUploads.contains(key);

    if (!m_messageModel->hasMessage(messageId)) {
      if (msgType == MessageModel::MESSAGE_TYPE::AUDIO) {
        m_messageModel->addPendingAudioMessage(messageId, serverId, contactPubKey,
                                              previewText,
                                              filemessage::serialize(fileData),
                                              timestamp);
      } else {
        m_messageModel->addPendingFileMessage(messageId, serverId, contactPubKey,
                                             previewText,
                                             filemessage::serialize(fileData),
                                             timestamp);
      }
    }

    if (alreadyRunning) {
      setTransferProgress(serverId, contactPubKey, messageId, tr("Uploading"),
                          0.0);
    } else if (canUpload) {
      auto* upload = new PendingUpload();
      upload->serverId = serverId;
      upload->contactPubKey = contactPubKey;
      upload->messageId = messageId;
      upload->messageType = msgType;
      upload->encryptedPath = encryptedPath;
      upload->fileData = fileData;
      upload->timestamp = timestamp;
      upload->previewText = previewText;
      upload->uploadUrl = urlString;
      upload->accessToken = accessToken;
      m_activeUploads.insert(key, upload);
      setTransferProgress(serverId, contactPubKey, messageId, tr("Uploading"),
                          0.0);
      startUploadRequest(upload);
    } else {
      m_messageModel->setFileTransferState(serverId, contactPubKey, messageId,
                                           false, false, 0.0, QString());
    }
  }
}

void FileTransferManager::onMessageDeleteRequested(
    quint64 messageId, const QString& serverId, const QString& contactPubKey) {
  // Cancel active upload (if uploading right now)
  cancelUpload(serverId, contactPubKey, messageId);

  // Clean up queued (not yet uploading) pending file
  const QString encryptedPath =
      m_db->pendingFileUploadEncryptedPath(messageId, serverId);
  if (!encryptedPath.isEmpty()) {
    QFile::remove(encryptedPath);
    m_db->deletePendingFileUpload(messageId, serverId);
  }
}

QString FileTransferManager::filesDirectory() const {
  return apppaths::filesDirectory();
}

QString FileTransferManager::makeUniqueTargetPath(
    const QString& preferredName) const {
  const QString directory = filesDirectory();
  QFileInfo info(sanitizeFileName(preferredName));
  const QString baseName = info.completeBaseName().isEmpty()
                               ? QStringLiteral("file")
                               : info.completeBaseName();
  const QString suffix = info.completeSuffix();

  QString candidate = directory + QLatin1Char('/') + info.fileName();
  if (!QFileInfo::exists(candidate)) {
    return candidate;
  }

  for (int index = 1; index < 10000; ++index) {
    const QString numberedName =
        suffix.isEmpty()
            ? QStringLiteral("%1_%2").arg(baseName).arg(index)
            : QStringLiteral("%1_%2.%3").arg(baseName).arg(index).arg(suffix);
    candidate = directory + QLatin1Char('/') + numberedName;
    if (!QFileInfo::exists(candidate)) {
      return candidate;
    }
  }

  return directory + QStringLiteral("/file_") +
         QString::number(QDateTime::currentMSecsSinceEpoch());
}

QString FileTransferManager::sanitizeFileName(const QString& fileName) const {
  QString sanitized = fileName;
  sanitized.replace('/', '_');
  sanitized.replace('\\', '_');
  if (sanitized.isEmpty()) {
    sanitized = QStringLiteral("file.bin");
  }
  return sanitized;
}

QString FileTransferManager::buildFilesUrl(const QString& serverId,
                                           const QString& fileId) const {
  const QString baseUrl =
      m_connectionManager->getHttpsBaseUrl(serverId).trimmed();
  if (baseUrl.isEmpty()) {
    return QString();
  }

  QString url = baseUrl;
  if (url.endsWith('/')) {
    url.chop(1);
  }

  url += QStringLiteral("/files");
  if (!fileId.isEmpty()) {
    url += QStringLiteral("/") + fileId;
  }

  return url;
}

QString FileTransferManager::transferKey(const QString& serverId,
                                         const QString& contactPubKey,
                                         quint64 messageId) const {
  return serverId + QLatin1Char(':') + contactPubKey + QLatin1Char(':') +
         QString::number(messageId);
}

void FileTransferManager::applySslPolicy(QNetworkReply* reply) const {
  if (reply == nullptr) {
    return;
  }

  if (m_settingsManager == nullptr ||
      !m_settingsManager->getBoolSetting("ignoreSslErrors", false)) {
    return;
  }

  connect(reply, &QNetworkReply::sslErrors, reply,
          [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
}

void FileTransferManager::applySslPolicy(QNetworkRequest& request) const {
  const bool ignoreSslErrors =
      m_settingsManager != nullptr &&
      m_settingsManager->getBoolSetting("ignoreSslErrors", false);
  qWarning() << "[FileTransferManager] [applySslPolicy] ignoreSslErrors setting="
             << ignoreSslErrors << "url=" << request.url();
  if (!ignoreSslErrors) {
    return;
  }

  QSslConfiguration sslConfig = request.sslConfiguration();
  sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
  request.setSslConfiguration(sslConfig);
  qWarning() << "[FileTransferManager] [applySslPolicy] VerifyNone applied"
             << "verifyMode=" << sslConfig.peerVerifyMode();
}

void FileTransferManager::updateMessageLocalPath(const QString& serverId,
                                                 const QString& contactPubKey,
                                                 quint64 messageId,
                                                 const QString& localPath) {
  QByteArray content =
      m_db->getMessageContent(serverId, contactPubKey, messageId);
  FileMessageData fileData;
  if (!filemessage::deserialize(content, &fileData)) {
    return;
  }

  fileData.localPath = localPath;
  if (filemessage::isAudioMessage(fileData) && fileData.waveform.isEmpty()) {
    fileData.waveform = voiceclip::buildWaveformFromFile(localPath);
  }

  const QByteArray updated = filemessage::serialize(fileData);
  m_db->updateMessageContent(serverId, contactPubKey, messageId, updated);
  m_messageModel->updateFileLocalPath(serverId, contactPubKey, messageId,
                                      localPath);
}

void FileTransferManager::downloadToDownloads(const QString& serverId,
                                              const QString& contactPubKey,
                                              quint64 messageId) {
  const QByteArray content =
      m_db->getMessageContent(serverId, contactPubKey, messageId);
  if (content.isEmpty()) {
    return;
  }

  AlbumMessageData album;
  if (filemessage::deserializeAlbum(content, &album) && !album.items.isEmpty()) {
    for (int i = 0; i < album.items.size(); ++i) {
      const FileMessageData& item = album.items[i];
      if (filemessage::hasLocalFile(item)) {
        emit saveToDownloadsRequested(item.localPath, item.fileName);
      } else {
        const QString key = transferKey(serverId, contactPubKey, messageId) +
                            QStringLiteral("#") + QString::number(i);
        m_saveAfterDownload.insert(key);
        downloadAlbumItem(serverId, contactPubKey, messageId, i);
      }
    }
    return;
  }

  FileMessageData fileData;
  if (!filemessage::deserialize(content, &fileData)) {
    return;
  }
  if (filemessage::hasLocalFile(fileData)) {
    emit saveToDownloadsRequested(fileData.localPath, fileData.fileName);
    return;
  }
  m_saveAfterDownload.insert(transferKey(serverId, contactPubKey, messageId));
  downloadFile(serverId, contactPubKey, messageId);
}

bool FileTransferManager::copyToDownloads(const QString& localPath,
                                          const QString& displayName) {
  const QFileInfo src(localPath);
  if (!src.exists() || !src.isFile()) {
    return false;
  }

  QString dir =
      QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (dir.isEmpty()) {
    dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
  }
  if (dir.isEmpty()) {
    return false;
  }
  QDir().mkpath(dir);

  QString safeName =
      sanitizeFileName(displayName.isEmpty() ? src.fileName() : displayName);
  if (safeName.isEmpty()) {
    safeName = QStringLiteral("file");
  }

  QString target = dir + QLatin1Char('/') + safeName;
  if (QFileInfo::exists(target)) {
    const QFileInfo nameInfo(safeName);
    const QString stem = nameInfo.completeBaseName();
    const QString suffix = nameInfo.suffix();
    int counter = 1;
    do {
      QString candidate = stem + QStringLiteral(" (") +
                          QString::number(counter) + QStringLiteral(")");
      if (!suffix.isEmpty()) {
        candidate += QLatin1Char('.') + suffix;
      }
      target = dir + QLatin1Char('/') + candidate;
      ++counter;
    } while (QFileInfo::exists(target));
  }

  return QFile::copy(localPath, target);
}

void FileTransferManager::updateAlbumItemLocalPath(
    const QString& serverId, const QString& contactPubKey, quint64 messageId,
    int itemIndex, const QString& localPath) {
  QByteArray content =
      m_db->getMessageContent(serverId, contactPubKey, messageId);
  AlbumMessageData album;
  if (!filemessage::deserializeAlbum(content, &album)) {
    return;
  }
  if (itemIndex < 0 || itemIndex >= album.items.size()) {
    return;
  }

  album.items[itemIndex].localPath = localPath;
  const QByteArray updated = filemessage::serializeAlbum(album);
  m_db->updateMessageContent(serverId, contactPubKey, messageId, updated);
  m_messageModel->updateAlbumItemLocalPath(serverId, contactPubKey, messageId,
                                           itemIndex, localPath);
}

void FileTransferManager::setTransferState(const QString& serverId,
                                           const QString& contactPubKey,
                                           quint64 messageId, qreal progress,
                                           const QString& statusText) {
  m_messageModel->setFileTransferState(serverId, contactPubKey, messageId, true,
                                       true, progress, statusText);
}

void FileTransferManager::setTransferProgress(const QString& serverId,
                                              const QString& contactPubKey,
                                              quint64 messageId,
                                              const QString& actionText,
                                              qreal progress) {
  setTransferState(serverId, contactPubKey, messageId, progress,
                   tr("%1 %2%").arg(actionText).arg(qRound(progress * 100.0)));
}

void FileTransferManager::postInfoMessage(const QString& serverId,
                                          const QString& contactPubKey,
                                          const QString& text) {
  if (contactPubKey.isEmpty() || text.isEmpty()) {
    return;
  }

  const quint64 messageId = m_db->addInfoMessage(serverId, contactPubKey, text);
  if (messageId == static_cast<quint64>(-1)) {
    qWarning() << "[FileTransferManager] [postInfoMessage] Failed to persist "
                  "info message"
               << "contactPubKey=" << contactPubKey << "text=" << text;
    return;
  }

  m_messageModel->addInfoMessage(serverId, contactPubKey, text);
  m_contactsModel->setLastMessageNow(serverId, contactPubKey, text);
}

void FileTransferManager::cleanupDownload(PendingBlobDownload* download) {
  if (download == nullptr) {
    return;
  }

  const QString key =
      download->downloadKey.isEmpty()
          ? transferKey(download->serverId, download->contactPubKey,
                        download->messageId)
          : download->downloadKey;
  m_activeDownloads.remove(key);

  if (download->workerThread != nullptr) {
    download->workerThread->quit();
  }

  download->worker = nullptr;
  download->workerThread = nullptr;
  delete download;
}

void FileTransferManager::failDownload(PendingBlobDownload* download,
                                       const QString& errorText) {
  qWarning() << "[FileTransferManager] [failDownload] Failing download"
             << "messageId=" << download->messageId << "errorText=" << errorText
             << "worker=" << download->worker
             << "canceled=" << download->canceled;

  if (download->albumIndex >= 0) {
    m_messageModel->setAlbumItemDownloading(
        download->serverId, download->contactPubKey, download->messageId,
        download->albumIndex, false, 0.0);
  } else {
    clearTransferState(download->serverId, download->contactPubKey,
                       download->messageId);
  }
  if (!errorText.isEmpty()) {
    postInfoMessage(download->serverId, download->contactPubKey, errorText);
  }

  cleanupDownload(download);
}

void FileTransferManager::finishDownload(PendingBlobDownload* download) {
  if (download->albumIndex >= 0) {
    updateAlbumItemLocalPath(download->serverId, download->contactPubKey,
                             download->messageId, download->albumIndex,
                             download->finalPath);
  } else {
    updateMessageLocalPath(download->serverId, download->contactPubKey,
                           download->messageId, download->finalPath);
    clearTransferState(download->serverId, download->contactPubKey,
                       download->messageId);
  }
  if (m_saveAfterDownload.remove(download->downloadKey)) {
    emit saveToDownloadsRequested(download->finalPath,
                                  download->fileData.fileName);
  }
  cleanupDownload(download);
}

void FileTransferManager::clearTransferState(const QString& serverId,
                                             const QString& contactPubKey,
                                             quint64 messageId) {
  m_messageModel->setFileTransferState(serverId, contactPubKey, messageId,
                                       false, false, 1.0, QString());
}