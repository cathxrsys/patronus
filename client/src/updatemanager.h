#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class AndroidSystemUi;
class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class SettingsManager;

class UpdateManager : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool repositoryConfigured READ repositoryConfigured NOTIFY
                 repositoryConfiguredChanged)
  Q_PROPERTY(QString repository READ repository NOTIFY repositoryChanged)
  Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
  Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged)
  Q_PROPERTY(
      QString currentProtocolVersion READ currentProtocolVersion CONSTANT)
  Q_PROPERTY(QString latestProtocolVersion READ latestProtocolVersion NOTIFY
                 stateChanged)
  Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY stateChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
  Q_PROPERTY(
      QString downloadedFilePath READ downloadedFilePath NOTIFY stateChanged)
  Q_PROPERTY(
      QString downloadedFileName READ downloadedFileName NOTIFY stateChanged)
  Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
  Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY stateChanged)
  Q_PROPERTY(bool protocolMismatch READ protocolMismatch NOTIFY stateChanged)
  Q_PROPERTY(
      bool downloadInProgress READ downloadInProgress NOTIFY stateChanged)
  Q_PROPERTY(bool downloadReady READ downloadReady NOTIFY stateChanged)
  Q_PROPERTY(bool canInstall READ canInstall NOTIFY stateChanged)
  Q_PROPERTY(qreal downloadProgress READ downloadProgress NOTIFY stateChanged)
  Q_PROPERTY(qint64 downloadBytesReceived READ downloadBytesReceived NOTIFY
                 stateChanged)
  Q_PROPERTY(
      qint64 downloadBytesTotal READ downloadBytesTotal NOTIFY stateChanged)

 public:
  explicit UpdateManager(SettingsManager* settingsManager,
                         AndroidSystemUi* androidSystemUi,
                         QObject* parent = nullptr);

  bool repositoryConfigured() const;
  QString repository() const;
  QString currentVersion() const;
  QString latestVersion() const;
  QString currentProtocolVersion() const;
  QString latestProtocolVersion() const;
  QString releaseNotes() const;
  QString statusText() const;
  QString downloadedFilePath() const;
  QString downloadedFileName() const;
  bool checking() const;
  bool updateAvailable() const;
  bool protocolMismatch() const;
  bool downloadInProgress() const;
  bool downloadReady() const;
  bool canInstall() const;
  qreal downloadProgress() const;
  qint64 downloadBytesReceived() const;
  qint64 downloadBytesTotal() const;

  Q_INVOKABLE void refresh();
  Q_INVOKABLE void downloadUpdate();
  Q_INVOKABLE void cancelDownload();
  Q_INVOKABLE bool installUpdate();

 signals:
  void stateChanged();
  void repositoryConfiguredChanged();
  void repositoryChanged();
  void errorOccurred(const QString& message);

 private:
  struct ArtifactInfo {
    QString url;
    QString fileName;

    bool isValid() const { return !url.isEmpty(); }
  };

  void updateRepositoryFromSettings();
  void setStatusText(const QString& text);
  void resetCheckState();
  void resetDownloadState();
  void applySslPolicy(QNetworkReply* reply) const;
  void beginManifestRequest(const QString& manifestUrl);
  void handleReleaseReply(QNetworkReply* reply);
  void handleManifestReply(QNetworkReply* reply);
  void startDownloadInternal();
  void handleDownloadFinished();
  void failWithMessage(const QString& message);
  QString defaultFileNameForUrl(const QString& url) const;
  QString updatesDirectory() const;
  bool isNewerVersion(const QString& candidateVersion,
                      const QString& installedVersion) const;
  ArtifactInfo parseArtifactInfo(const QJsonObject& manifestObject) const;
  QString parseString(const QJsonObject& object, const QStringList& keys) const;
  QString parseNotes(const QJsonObject& manifestObject) const;
  QString manifestPlatformKey() const;
  bool installDownloadedFileDesktop();
  bool installDownloadedFileAndroid();
  bool installDownloadedFileLinux();
    bool launchWindowsReplacementScript(const QString& sourcePath,
                                                                            const QString& targetPath);
  bool launchLinuxReplacementScript(const QString& sourcePath,
                                    const QString& targetPath);

  SettingsManager* m_settingsManager = nullptr;
  AndroidSystemUi* m_androidSystemUi = nullptr;
  QNetworkAccessManager* m_networkManager = nullptr;
  QPointer<QNetworkReply> m_releaseReply;
  QPointer<QNetworkReply> m_manifestReply;
  QPointer<QNetworkReply> m_downloadReply;
  QFile* m_downloadFile = nullptr;

  QString m_repository;
  QString m_statusText;
  QString m_latestVersion;
  QString m_latestProtocolVersion;
  QString m_releaseNotes;
  QString m_manifestUrl;
  QString m_downloadedFilePath;
  QString m_downloadedFileName;
  QString m_pendingDownloadPath;
  ArtifactInfo m_artifact;
  bool m_checking = false;
  bool m_updateAvailable = false;
  bool m_protocolMismatch = false;
  bool m_downloadInProgress = false;
  qint64 m_downloadBytesReceived = 0;
  qint64 m_downloadBytesTotal = 0;
};