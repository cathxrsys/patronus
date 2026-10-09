#include "apppaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace apppaths {

namespace {

QString ensureDirectory(const QString& path) {
  if (path.isEmpty()) {
    return {};
  }

  QDir directory(path);
  if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
    return {};
  }

  return directory.absolutePath();
}

QString childDirectory(const QString& parent, const QString& name) {
  if (parent.isEmpty()) {
    return {};
  }

  return ensureDirectory(QDir(parent).filePath(name));
}

QString portableBaseDirectory() {
  const QString appImagePath = qEnvironmentVariable("APPIMAGE");
  if (!appImagePath.isEmpty()) {
    return QFileInfo(appImagePath).absolutePath();
  }

  return QCoreApplication::applicationDirPath();
}

}  // namespace

QString appDataDirectory() {
#ifdef Q_OS_ANDROID
  QString path =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (path.isEmpty()) {
    path =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  }

  if (path.isEmpty()) {
    path = QCoreApplication::applicationDirPath();
  }
#else
  const QString path = childDirectory(portableBaseDirectory(),
                                      QStringLiteral("PatronusData"));
#endif

  return ensureDirectory(path);
}

QString filesDirectory() {
  return childDirectory(appDataDirectory(), QStringLiteral("files"));
}

QString tempDirectory() {
  return childDirectory(appDataDirectory(), QStringLiteral("tmp"));
}

QString databasePath() {
  const QString baseDirectory = appDataDirectory();
  if (baseDirectory.isEmpty()) {
    return QStringLiteral("main.db");
  }

  return QDir(baseDirectory).filePath(QStringLiteral("main.db"));
}

QString settingsFilePath() {
  const QString baseDirectory = appDataDirectory();
  if (baseDirectory.isEmpty()) {
    return QStringLiteral("settings.ini");
  }

  return QDir(baseDirectory).filePath(QStringLiteral("settings.ini"));
}

}  // namespace apppaths