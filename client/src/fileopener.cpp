#include "fileopener.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>

namespace fileopener {

bool openLocalFile(const QString& localPath) {
  if (localPath.isEmpty()) {
    return false;
  }

  const QFileInfo info(localPath);
  if (!info.exists()) {
    return false;
  }

  return QDesktopServices::openUrl(
      QUrl::fromLocalFile(info.absoluteFilePath()));
}

bool openContainingFolder(const QString& localPath) {
  if (localPath.isEmpty()) {
    return false;
  }

  const QFileInfo info(localPath);
  if (!info.exists()) {
    return false;
  }

#ifdef Q_OS_ANDROID
  Q_UNUSED(localPath);
  return false;
#else
  return QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
#endif
}

}  // namespace fileopener