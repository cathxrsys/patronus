#include "updatemanager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSslError>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QUrl>
#include <QVersionNumber>

#include "androidsystemui.h"
#include "apppaths.h"
#include "appversion.h"
#include "settingsmanager.h"

namespace {

constexpr auto kDefaultManifestAssetName = "manifest.json";
constexpr auto kGitHubApiBase = "https://api.github.com/repos/";
constexpr auto kLatestReleaseSuffix = "/releases/latest";

QString trimTrailingSlash(QString value) {
  while (value.endsWith('/')) {
    value.chop(1);
  }

  return value;
}

QString sanitizeFileName(QString value) {
  value.replace('\\', '_');
  value.replace('/', '_');
  value.replace(':', '_');
  return value;
}

QString jsonValueToString(const QJsonValue& value) {
  if (value.isString()) {
    return value.toString().trimmed();
  }

  if (value.isDouble()) {
    return QString::number(value.toInteger());
  }

  if (value.isBool()) {
    return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  }

  return {};
}

QString firstExistingPath(const QStringList& candidates) {
  for (const QString& candidate : candidates) {
    if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
      return candidate;
    }
  }

  return {};
}

}  // namespace

UpdateManager::UpdateManager(SettingsManager* settingsManager,
                             AndroidSystemUi* androidSystemUi, QObject* parent)
    : QObject(parent),
      m_settingsManager(settingsManager),
      m_androidSystemUi(androidSystemUi),
      m_networkManager(new QNetworkAccessManager(this)),
      m_statusText(tr("Update check has not started yet")) {
  updateRepositoryFromSettings();

  if (m_settingsManager != nullptr) {
    connect(m_settingsManager, &SettingsManager::settingChanged, this,
            [this](const QString& settingName, const QString&) {
              if (settingName == QStringLiteral("updatesRepository")) {
                updateRepositoryFromSettings();
              }
            });
  }
}

bool UpdateManager::repositoryConfigured() const {
  return !m_repository.isEmpty();
}

QString UpdateManager::repository() const { return m_repository; }

QString UpdateManager::currentVersion() const {
  return QString::fromUtf8(appversion::kApplicationVersion);
}

QString UpdateManager::latestVersion() const { return m_latestVersion; }

QString UpdateManager::currentProtocolVersion() const {
  return QString::fromUtf8(appversion::kProtocolVersion);
}

QString UpdateManager::latestProtocolVersion() const {
  return m_latestProtocolVersion;
}

QString UpdateManager::releaseNotes() const { return m_releaseNotes; }

QString UpdateManager::statusText() const { return m_statusText; }

QString UpdateManager::downloadedFilePath() const {
  return m_downloadedFilePath;
}

QString UpdateManager::downloadedFileName() const {
  return m_downloadedFileName;
}

bool UpdateManager::checking() const { return m_checking; }

bool UpdateManager::updateAvailable() const { return m_updateAvailable; }

bool UpdateManager::protocolMismatch() const { return m_protocolMismatch; }

bool UpdateManager::downloadInProgress() const { return m_downloadInProgress; }

bool UpdateManager::downloadReady() const {
  return !m_downloadedFilePath.isEmpty() &&
         QFileInfo::exists(m_downloadedFilePath);
}

bool UpdateManager::canInstall() const { return downloadReady(); }

qreal UpdateManager::downloadProgress() const {
  if (m_downloadBytesTotal <= 0) {
    return 0.0;
  }

  return qBound<qreal>(0.0,
                       static_cast<qreal>(m_downloadBytesReceived) /
                           static_cast<qreal>(m_downloadBytesTotal),
                       1.0);
}

qint64 UpdateManager::downloadBytesReceived() const {
  return m_downloadBytesReceived;
}

qint64 UpdateManager::downloadBytesTotal() const {
  return m_downloadBytesTotal;
}

void UpdateManager::refresh() {
  updateRepositoryFromSettings();
  resetCheckState();

  if (!repositoryConfigured()) {
    setStatusText(tr("Updates repository is not configured"));
    emit repositoryConfiguredChanged();
    return;
  }

  m_checking = true;
  emit stateChanged();

  if (m_repository.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
      m_repository.startsWith(QStringLiteral("https://"),
                              Qt::CaseInsensitive)) {
    beginManifestRequest(m_repository);
    return;
  }

  const QUrl releaseUrl(QString::fromLatin1(kGitHubApiBase) + m_repository +
                        QString::fromLatin1(kLatestReleaseSuffix));
  QNetworkRequest request(releaseUrl);
  request.setHeader(QNetworkRequest::UserAgentHeader,
                    QStringLiteral("patronus/%1 updater").arg(currentVersion()));
  request.setRawHeader("Accept", "application/vnd.github+json");

  m_releaseReply = m_networkManager->get(request);
  applySslPolicy(m_releaseReply);
  connect(m_releaseReply, &QNetworkReply::finished, this,
          [this, reply = m_releaseReply]() { handleReleaseReply(reply); });
}

void UpdateManager::downloadUpdate() {
  if (m_downloadInProgress || !m_updateAvailable || !m_artifact.isValid()) {
    return;
  }

  startDownloadInternal();
}

void UpdateManager::cancelDownload() {
  if (m_downloadReply != nullptr) {
    m_downloadReply->abort();
  }
}

bool UpdateManager::installUpdate() {
  if (!downloadReady()) {
    failWithMessage(tr("Downloaded file is missing"));
    return false;
  }

#ifdef Q_OS_ANDROID
  return installDownloadedFileAndroid();
#elif defined(Q_OS_WIN)
  return installDownloadedFileDesktop();
#elif defined(Q_OS_LINUX)
  return installDownloadedFileLinux();
#else
  return installDownloadedFileDesktop();
#endif
}

void UpdateManager::updateRepositoryFromSettings() {
  const QString oldRepository = m_repository;
  if (m_settingsManager != nullptr) {
    m_repository = trimTrailingSlash(
        m_settingsManager
            ->getTextSetting(QStringLiteral("updatesRepository"), QString())
            .trimmed());
  } else {
    m_repository.clear();
  }

  if (oldRepository != m_repository) {
    emit repositoryChanged();
    emit repositoryConfiguredChanged();
  }
}

void UpdateManager::setStatusText(const QString& text) {
  if (m_statusText == text) {
    return;
  }

  m_statusText = text;
  emit stateChanged();
}

void UpdateManager::resetCheckState() {
  if (m_releaseReply != nullptr) {
    m_releaseReply->abort();
    m_releaseReply->deleteLater();
  }
  if (m_manifestReply != nullptr) {
    m_manifestReply->abort();
    m_manifestReply->deleteLater();
  }

  m_releaseReply = nullptr;
  m_manifestReply = nullptr;
  m_checking = false;
  m_updateAvailable = false;
  m_protocolMismatch = false;
  m_latestVersion.clear();
  m_latestProtocolVersion.clear();
  m_releaseNotes.clear();
  m_manifestUrl.clear();
  m_artifact = {};
  emit stateChanged();
}

void UpdateManager::resetDownloadState() {
  if (m_downloadReply != nullptr) {
    m_downloadReply->abort();
    m_downloadReply->deleteLater();
    m_downloadReply = nullptr;
  }

  if (m_downloadFile != nullptr) {
    if (m_downloadFile->isOpen()) {
      m_downloadFile->close();
    }
    m_downloadFile->deleteLater();
    m_downloadFile = nullptr;
  }

  if (!m_pendingDownloadPath.isEmpty()) {
    QFile::remove(m_pendingDownloadPath);
    m_pendingDownloadPath.clear();
  }

  m_downloadInProgress = false;
  m_downloadBytesReceived = 0;
  m_downloadBytesTotal = 0;
  emit stateChanged();
}

void UpdateManager::applySslPolicy(QNetworkReply* reply) const {
  if (reply == nullptr || m_settingsManager == nullptr) {
    return;
  }

  if (!m_settingsManager->getBoolSetting(QStringLiteral("ignoreSslErrors"),
                                         false)) {
    return;
  }

  connect(reply, &QNetworkReply::sslErrors, reply,
          [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
}

void UpdateManager::beginManifestRequest(const QString& manifestUrl) {
  const QUrl url(manifestUrl);
  if (!url.isValid() || url.isEmpty()) {
    failWithMessage(tr("Manifest URL is invalid"));
    return;
  }

  m_manifestUrl = manifestUrl;

  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::UserAgentHeader,
                    QStringLiteral("patronus/%1 updater").arg(currentVersion()));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);

  m_manifestReply = m_networkManager->get(request);
  applySslPolicy(m_manifestReply);
  connect(m_manifestReply, &QNetworkReply::finished, this,
          [this, reply = m_manifestReply]() { handleManifestReply(reply); });

  setStatusText(tr("Checking for updates..."));
}

void UpdateManager::handleReleaseReply(QNetworkReply* reply) {
  if (reply == nullptr) {
    failWithMessage(tr("Release request failed"));
    return;
  }

  const QByteArray body = reply->readAll();
  const auto error = reply->error();
  const QString errorText = reply->errorString();
  reply->deleteLater();
  m_releaseReply = nullptr;

  if (error != QNetworkReply::NoError) {
    failWithMessage(
        tr("Failed to query GitHub release API: %1").arg(errorText));
    return;
  }

  const QJsonDocument jsonDocument = QJsonDocument::fromJson(body);
  if (!jsonDocument.isObject()) {
    failWithMessage(tr("GitHub release response is not valid JSON"));
    return;
  }

  const QJsonArray assets =
      jsonDocument.object().value(QStringLiteral("assets")).toArray();
  QString manifestUrl;
  for (const QJsonValue& assetValue : assets) {
    const QJsonObject assetObject = assetValue.toObject();
    const QString assetName =
        assetObject.value(QStringLiteral("name")).toString();
    if (assetName.compare(QString::fromLatin1(kDefaultManifestAssetName),
                          Qt::CaseInsensitive) == 0) {
      manifestUrl =
          assetObject.value(QStringLiteral("browser_download_url")).toString();
      if (manifestUrl.isEmpty()) {
        manifestUrl = assetObject.value(QStringLiteral("url")).toString();
      }
      break;
    }
  }

  if (manifestUrl.isEmpty()) {
    failWithMessage(
        tr("manifest.json was not found in the latest release assets"));
    return;
  }

  beginManifestRequest(manifestUrl);
}

void UpdateManager::handleManifestReply(QNetworkReply* reply) {
  if (reply == nullptr) {
    failWithMessage(tr("Manifest request failed"));
    return;
  }

  const QByteArray body = reply->readAll();
  const auto error = reply->error();
  const QString errorText = reply->errorString();
  reply->deleteLater();
  m_manifestReply = nullptr;

  if (error != QNetworkReply::NoError) {
    failWithMessage(tr("Failed to download manifest: %1").arg(errorText));
    return;
  }

  const QJsonDocument jsonDocument = QJsonDocument::fromJson(body);
  if (!jsonDocument.isObject()) {
    failWithMessage(tr("Manifest is not valid JSON"));
    return;
  }

  const QJsonObject manifestObject = jsonDocument.object();
  m_latestVersion = parseString(
      manifestObject, {QStringLiteral("version"), QStringLiteral("appVersion"),
                       QStringLiteral("latestVersion")});
  m_latestProtocolVersion = parseString(
      manifestObject,
      {QStringLiteral("protocolVersion"), QStringLiteral("protocol_version")});
  m_releaseNotes = parseNotes(manifestObject);
  m_artifact = parseArtifactInfo(manifestObject);
  m_updateAvailable = !m_latestVersion.isEmpty() &&
                      isNewerVersion(m_latestVersion, currentVersion());
  m_protocolMismatch = !m_latestProtocolVersion.isEmpty() &&
                       m_latestProtocolVersion != currentProtocolVersion();
  m_checking = false;

  if (m_latestVersion.isEmpty()) {
    failWithMessage(tr("Manifest does not contain an application version"));
    return;
  }

  if (!m_artifact.isValid()) {
    failWithMessage(
        tr("Manifest does not contain an artifact for this platform"));
    return;
  }

  if (m_updateAvailable) {
    setStatusText(tr("Update %1 is available").arg(m_latestVersion));
  } else {
    setStatusText(tr("You already have the latest version"));
  }

  emit stateChanged();
}

void UpdateManager::startDownloadInternal() {
  const QString directoryPath = updatesDirectory();
  if (directoryPath.isEmpty()) {
    failWithMessage(tr("Failed to prepare updates directory"));
    return;
  }

  resetDownloadState();

  m_downloadedFileName =
      !m_artifact.fileName.isEmpty()
          ? sanitizeFileName(m_artifact.fileName)
          : sanitizeFileName(defaultFileNameForUrl(m_artifact.url));
  if (m_downloadedFileName.isEmpty()) {
    m_downloadedFileName = QStringLiteral("update.bin");
  }

  const QString finalPath = QDir(directoryPath).filePath(m_downloadedFileName);
  m_pendingDownloadPath = finalPath + QStringLiteral(".part");
  QFile::remove(m_pendingDownloadPath);

  m_downloadFile = new QFile(m_pendingDownloadPath, this);
  if (!m_downloadFile->open(QIODevice::WriteOnly)) {
    m_downloadFile->deleteLater();
    m_downloadFile = nullptr;
    failWithMessage(tr("Failed to open target file for download"));
    return;
  }

  QNetworkRequest request(QUrl(m_artifact.url));
  request.setHeader(QNetworkRequest::UserAgentHeader,
                    QStringLiteral("patronus/%1 updater").arg(currentVersion()));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);

  m_downloadReply = m_networkManager->get(request);
  applySslPolicy(m_downloadReply);
  m_downloadInProgress = true;
  emit stateChanged();
  setStatusText(tr("Downloading update..."));

  connect(m_downloadReply, &QNetworkReply::readyRead, this, [this]() {
    if (m_downloadReply != nullptr && m_downloadFile != nullptr) {
      m_downloadFile->write(m_downloadReply->readAll());
    }
  });
  connect(m_downloadReply, &QNetworkReply::downloadProgress, this,
          [this](qint64 bytesReceived, qint64 bytesTotal) {
            m_downloadBytesReceived = bytesReceived;
            m_downloadBytesTotal = bytesTotal;
            emit stateChanged();
          });
  connect(m_downloadReply, &QNetworkReply::finished, this,
          &UpdateManager::handleDownloadFinished);
}

void UpdateManager::handleDownloadFinished() {
  if (m_downloadReply == nullptr) {
    resetDownloadState();
    return;
  }

  if (m_downloadFile != nullptr) {
    m_downloadFile->write(m_downloadReply->readAll());
    m_downloadFile->flush();
    m_downloadFile->close();
  }

  const auto error = m_downloadReply->error();
  const QString errorText = m_downloadReply->errorString();
  m_downloadReply->deleteLater();
  m_downloadReply = nullptr;

  if (error != QNetworkReply::NoError) {
    resetDownloadState();
    failWithMessage(tr("Failed to download update: %1").arg(errorText));
    return;
  }

  const QString finalPath =
      QDir(updatesDirectory()).filePath(m_downloadedFileName);
  QFile::remove(finalPath);
  if (!QFile::rename(m_pendingDownloadPath, finalPath)) {
    resetDownloadState();
    failWithMessage(tr("Failed to finalize the downloaded update file"));
    return;
  }

  m_pendingDownloadPath.clear();
  if (m_downloadFile != nullptr) {
    m_downloadFile->deleteLater();
    m_downloadFile = nullptr;
  }

  m_downloadedFilePath = finalPath;
  m_downloadInProgress = false;
  setStatusText(tr("Update downloaded and ready to install"));
  emit stateChanged();
}

void UpdateManager::failWithMessage(const QString& message) {
  m_checking = false;
  m_downloadInProgress = false;
  setStatusText(message);
  emit errorOccurred(message);
  emit stateChanged();
}

QString UpdateManager::defaultFileNameForUrl(const QString& url) const {
  const QUrl parsedUrl(url);
  const QString fileName = QFileInfo(parsedUrl.path()).fileName();
  return fileName;
}

QString UpdateManager::updatesDirectory() const {
  const QString basePath = apppaths::filesDirectory();
  if (basePath.isEmpty()) {
    return {};
  }

  QDir directory(basePath);
  const QString updatesPath = directory.filePath(QStringLiteral("updates"));
  if (!directory.mkpath(QStringLiteral("updates"))) {
    return {};
  }

  return updatesPath;
}

bool UpdateManager::isNewerVersion(const QString& candidateVersion,
                                   const QString& installedVersion) const {
  if (candidateVersion == installedVersion) {
    return false;
  }

  qsizetype candidateSuffixIndex = 0;
  qsizetype installedSuffixIndex = 0;
  const QVersionNumber candidate =
      QVersionNumber::fromString(candidateVersion, &candidateSuffixIndex);
  const QVersionNumber installed =
      QVersionNumber::fromString(installedVersion, &installedSuffixIndex);
  if (!candidate.isNull() && !installed.isNull()) {
    const int compareResult = QVersionNumber::compare(candidate, installed);
    if (compareResult != 0) {
      return compareResult > 0;
    }
  }

  return candidateVersion != installedVersion;
}

UpdateManager::ArtifactInfo UpdateManager::parseArtifactInfo(
    const QJsonObject& manifestObject) const {
  ArtifactInfo artifactInfo;
  const QString platformKey = manifestPlatformKey();

  auto parseArtifactObject = [this](const QJsonObject& object) {
    ArtifactInfo info;
    info.url = parseString(
        object, {QStringLiteral("url"), QStringLiteral("browser_download_url"),
                 QStringLiteral("downloadUrl")});
    info.fileName = parseString(
        object, {QStringLiteral("fileName"), QStringLiteral("filename"),
                 QStringLiteral("name")});
    return info;
  };

  const QJsonValue artifactsValue =
      manifestObject.value(QStringLiteral("artifacts"));
  if (artifactsValue.isObject()) {
    const QJsonObject artifactsObject = artifactsValue.toObject();
    const QStringList keysToTry =
        platformKey == QStringLiteral("linux")
            ? QStringList{QStringLiteral("linux"), QStringLiteral("appimage")}
            : QStringList{platformKey};
    for (const QString& key : keysToTry) {
      const QJsonValue candidate = artifactsObject.value(key);
      if (candidate.isObject()) {
        artifactInfo = parseArtifactObject(candidate.toObject());
        if (artifactInfo.isValid()) {
          return artifactInfo;
        }
      }
    }
  }

  if (artifactsValue.isArray()) {
    const QJsonArray artifactsArray = artifactsValue.toArray();
    for (const QJsonValue& artifactValue : artifactsArray) {
      const QJsonObject artifactObject = artifactValue.toObject();
      const QString artifactPlatform = parseString(
          artifactObject, {QStringLiteral("platform"), QStringLiteral("os")});
      if (artifactPlatform.compare(platformKey, Qt::CaseInsensitive) == 0) {
        artifactInfo = parseArtifactObject(artifactObject);
        if (artifactInfo.isValid()) {
          return artifactInfo;
        }
      }
    }
  }

  const QStringList directKeys =
      platformKey == QStringLiteral("linux")
          ? QStringList{QStringLiteral("linux"), QStringLiteral("appimage")}
          : QStringList{platformKey};
  for (const QString& key : directKeys) {
    const QJsonValue directValue = manifestObject.value(key);
    if (directValue.isObject()) {
      artifactInfo = parseArtifactObject(directValue.toObject());
      if (artifactInfo.isValid()) {
        return artifactInfo;
      }
    }
  }

  return artifactInfo;
}

QString UpdateManager::parseString(const QJsonObject& object,
                                   const QStringList& keys) const {
  for (const QString& key : keys) {
    const QString value = jsonValueToString(object.value(key));
    if (!value.isEmpty()) {
      return value;
    }
  }

  return {};
}

QString UpdateManager::parseNotes(const QJsonObject& manifestObject) const {
  QString notes = parseString(
      manifestObject, {QStringLiteral("notes"), QStringLiteral("releaseNotes"),
                       QStringLiteral("description")});
  if (!notes.isEmpty()) {
    return notes;
  }

  const QJsonArray notesArray =
      manifestObject.value(QStringLiteral("notes")).toArray();
  QStringList lines;
  for (const QJsonValue& noteValue : notesArray) {
    const QString line = jsonValueToString(noteValue);
    if (!line.isEmpty()) {
      lines.append(line);
    }
  }

  return lines.join('\n');
}

QString UpdateManager::manifestPlatformKey() const {
#ifdef Q_OS_ANDROID
  return QStringLiteral("android");
#elif defined(Q_OS_WIN)
  return QStringLiteral("windows");
#elif defined(Q_OS_LINUX)
  return QStringLiteral("linux");
#else
  return QSysInfo::productType().toLower();
#endif
}

bool UpdateManager::installDownloadedFileDesktop() {
  const QString targetPath = QCoreApplication::applicationFilePath();
  if (targetPath.isEmpty()) {
    failWithMessage(tr("Failed to resolve current executable path"));
    return false;
  }

  if (!launchWindowsReplacementScript(m_downloadedFilePath, targetPath)) {
    failWithMessage(tr("Failed to prepare Windows executable replacement"));
    return false;
  }

  setStatusText(tr("Windows executable replacement started"));
  QCoreApplication::quit();
  return true;
}

bool UpdateManager::installDownloadedFileAndroid() {
  if (m_androidSystemUi == nullptr || !m_androidSystemUi->isAvailable()) {
    failWithMessage(tr("Android installer is unavailable"));
    return false;
  }

  const bool started = m_androidSystemUi->installApk(m_downloadedFilePath);
  if (!started) {
    failWithMessage(tr("Failed to open Android package installer"));
    return false;
  }

  setStatusText(tr("Android package installer opened"));
  return true;
}

bool UpdateManager::installDownloadedFileLinux() {
  const QString appImagePath = qEnvironmentVariable("APPIMAGE");
  QString targetPath;
  if (!appImagePath.isEmpty()) {
    targetPath = appImagePath;
  } else {
    const QFileInfo currentBinaryInfo(QCoreApplication::applicationFilePath());
    const QDir currentDirectory = currentBinaryInfo.absoluteDir();
    const QStringList appImages =
        currentDirectory.entryList({QStringLiteral("*.AppImage")}, QDir::Files);
    if (!appImages.isEmpty()) {
      targetPath = currentDirectory.filePath(appImages.constFirst());
    } else {
      targetPath = currentDirectory.filePath(
          m_downloadedFileName.isEmpty() ? QStringLiteral("patronus.AppImage")
                                         : m_downloadedFileName);
    }
  }

  if (!launchLinuxReplacementScript(m_downloadedFilePath, targetPath)) {
    failWithMessage(tr("Failed to prepare AppImage replacement"));
    return false;
  }

  setStatusText(tr("AppImage replacement started"));
  QCoreApplication::quit();
  return true;
}

bool UpdateManager::launchWindowsReplacementScript(const QString& sourcePath,
                                                   const QString& targetPath) {
  QTemporaryFile scriptFile(
      QDir(apppaths::tempDirectory())
          .filePath(QStringLiteral("update-install-XXXXXX.cmd")));
  scriptFile.setAutoRemove(false);
  if (!scriptFile.open()) {
    return false;
  }

  const QString sourceNative = QDir::toNativeSeparators(sourcePath);
  const QString targetNative = QDir::toNativeSeparators(targetPath);

  QTextStream stream(&scriptFile);
  stream << "@echo off\r\n";
  stream << "setlocal enableextensions\r\n";
  stream << "set \"SOURCE=" << sourceNative << "\"\r\n";
  stream << "set \"TARGET=" << targetNative << "\"\r\n";
  stream << "set \"TARGET_TMP=%TARGET%.new\"\r\n";
  stream << "set \"OLD_PID=" << QCoreApplication::applicationPid()
         << "\"\r\n";
  stream << ":wait\r\n";
  stream << "tasklist /FI \"PID eq %OLD_PID%\" 2>NUL | find \"%OLD_PID%\" >NUL\r\n";
  stream << "if not errorlevel 1 (\r\n";
  stream << "  timeout /T 1 /NOBREAK >NUL\r\n";
  stream << "  goto wait\r\n";
  stream << ")\r\n";
  stream << "if not exist \"%SOURCE%\" exit /b 1\r\n";
  stream << "for %%I in (\"%TARGET%\") do if not exist \"%%~dpI\" mkdir \"%%~dpI\"\r\n";
  stream << "copy /Y \"%SOURCE%\" \"%TARGET_TMP%\" >NUL || exit /b 1\r\n";
  stream << "move /Y \"%TARGET_TMP%\" \"%TARGET%\" >NUL || exit /b 1\r\n";
  stream << "start \"\" \"%TARGET%\"\r\n";
  stream << "del /Q \"%SOURCE%\" 2>NUL\r\n";
  stream << "start \"\" /B cmd /c del /Q \"%~f0\"\r\n";
  stream.flush();
  scriptFile.close();

  return QProcess::startDetached(QStringLiteral("cmd.exe"),
                                 {QStringLiteral("/C"), scriptFile.fileName()});
}

bool UpdateManager::launchLinuxReplacementScript(const QString& sourcePath,
                                                 const QString& targetPath) {
  QTemporaryFile scriptFile(
      QDir(apppaths::tempDirectory())
          .filePath(QStringLiteral("update-install-XXXXXX.sh")));
  scriptFile.setAutoRemove(false);
  if (!scriptFile.open()) {
    return false;
  }

  QTextStream stream(&scriptFile);
  stream << "#!/bin/sh\n";
  stream << "set -eu\n";
  stream << "SOURCE=\"" << sourcePath << "\"\n";
  stream << "TARGET=\"" << targetPath << "\"\n";
  stream << "TARGET_TMP=\"${TARGET}.new\"\n";
  stream << "OLD_PID=\"" << QCoreApplication::applicationPid() << "\"\n";
  stream << "while kill -0 \"${OLD_PID}\" 2>/dev/null; do sleep 1; done\n";
  stream << "mkdir -p \"$(dirname \"${TARGET}\")\"\n";
  stream << "cp \"${SOURCE}\" \"${TARGET_TMP}\"\n";
  stream << "chmod +x \"${TARGET_TMP}\"\n";
  stream << "mv -f \"${TARGET_TMP}\" \"${TARGET}\"\n";
  stream << "nohup \"${TARGET}\" >/dev/null 2>&1 &\n";
  stream << "rm -f \"$0\"\n";
  stream.flush();
  scriptFile.close();

  QFile::setPermissions(scriptFile.fileName(),
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                            QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                            QFileDevice::ExeGroup | QFileDevice::ReadOther |
                            QFileDevice::ExeOther);

  return QProcess::startDetached(QStringLiteral("/bin/sh"),
                                 {scriptFile.fileName()});
}