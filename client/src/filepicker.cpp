#include "filepicker.h"

#include <QCoreApplication>
#include <QDir>

#include "apppaths.h"
#ifdef Q_OS_ANDROID
#include <QtCore/qcoreapplication_platform.h>

#include <QJniEnvironment>
#include <QJniObject>
#include <QMutex>
#include <QPointer>
#else
#include <QFileDialog>
#endif
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>

namespace filepicker {

namespace {

#ifdef Q_OS_ANDROID
QMutex g_callbackMutex;
QPointer<QObject> g_callbackContext;
SelectionCallback g_selectionCallback;
QPointer<QObject> g_multiCallbackContext;
MultiSelectionCallback g_multiSelectionCallback;
#endif

QString sanitizeFileName(QString fileName) {
  fileName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                   QStringLiteral("_"));
  return fileName;
}

#ifdef Q_OS_ANDROID
// Moves a file Java already streamed to its own cache dir (see
// PatronusActivity.copyPickedUriToCache) into the app's tmp directory under a
// collision-proof name — a plain filesystem rename/copy, so the content is
// never held in memory here (unlike the old byte-array pipeline, which
// buffered whole files on both the Java and native sides and could exhaust
// the heap on a large video or multi-file album pick).
QString persistPickedFile(const QString& fileNameHint,
                          const QString& sourcePath) {
  if (sourcePath.isEmpty()) {
    return {};
  }

  const QString tempDirectoryPath = apppaths::tempDirectory();
  if (tempDirectoryPath.isEmpty()) {
    QFile::remove(sourcePath);
    return {};
  }

  QDir tempDirectory(tempDirectoryPath);
  if (!tempDirectory.exists() && !tempDirectory.mkpath(QStringLiteral("."))) {
    QFile::remove(sourcePath);
    return {};
  }

  const QFileInfo sourceInfo(fileNameHint);
  QString baseName = sanitizeFileName(sourceInfo.completeBaseName());
  if (baseName.isEmpty()) {
    baseName = QStringLiteral("picked_file");
  }

  const QString suffix = sanitizeFileName(sourceInfo.suffix());
  QString targetName = baseName + QLatin1Char('_') +
                       QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (!suffix.isEmpty()) {
    targetName += QLatin1Char('.') + suffix;
  }

  const QString targetPath = tempDirectory.filePath(targetName);
  if (QFile::rename(sourcePath, targetPath)) {
    return targetPath;
  }

  // Cross-filesystem (cache dir vs app data dir can be different mounts):
  // rename() fails there, fall back to a real copy.
  if (QFile::copy(sourcePath, targetPath)) {
    QFile::remove(sourcePath);
    return targetPath;
  }

  QFile::remove(sourcePath);
  return {};
}

void completeSelectionOnQtThread(QString filePath) {
  SelectionCallback callback;
  QPointer<QObject> context;
  {
    QMutexLocker locker(&g_callbackMutex);
    callback = std::move(g_selectionCallback);
    context = g_callbackContext;
    g_callbackContext.clear();
  }

  if (!callback) {
    return;
  }

  if (!context) {
    return;
  }

  callback(filePath);
}

void completeSelectionFromAndroid(const QString& fileNameHint,
                                  const QString& sourcePath) {
  QObject* application = QCoreApplication::instance();
  if (application == nullptr) {
    completeSelectionOnQtThread(QString());
    return;
  }

  const QString persistedFilePath =
      fileNameHint.isEmpty() ? QString()
                             : persistPickedFile(fileNameHint, sourcePath);
  QMetaObject::invokeMethod(
      application,
      [persistedFilePath]() { completeSelectionOnQtThread(persistedFilePath); },
      Qt::QueuedConnection);
}

void notifyAndroidSelectionCancelled() {
  QObject* application = QCoreApplication::instance();
  if (application == nullptr) {
    completeSelectionOnQtThread(QString());
    return;
  }

  QMetaObject::invokeMethod(
      application, []() { completeSelectionOnQtThread(QString()); },
      Qt::QueuedConnection);
}

void completeMultiSelectionOnQtThread(QStringList filePaths) {
  MultiSelectionCallback callback;
  QPointer<QObject> context;
  {
    QMutexLocker locker(&g_callbackMutex);
    callback = std::move(g_multiSelectionCallback);
    context = g_multiCallbackContext;
    g_multiCallbackContext.clear();
  }

  if (!callback || !context) {
    return;
  }

  callback(filePaths);
}

void completeMultiSelectionFromAndroid(const QStringList& fileNames,
                                       const QStringList& sourcePaths) {
  QStringList persistedPaths;
  const int count = std::min(fileNames.size(), sourcePaths.size());
  for (int i = 0; i < count; ++i) {
    if (fileNames.at(i).isEmpty() || sourcePaths.at(i).isEmpty()) {
      continue;
    }
    const QString path = persistPickedFile(fileNames.at(i), sourcePaths.at(i));
    if (!path.isEmpty()) {
      persistedPaths.append(path);
    }
  }

  QObject* application = QCoreApplication::instance();
  if (application == nullptr) {
    completeMultiSelectionOnQtThread(persistedPaths);
    return;
  }

  QMetaObject::invokeMethod(
      application,
      [persistedPaths]() { completeMultiSelectionOnQtThread(persistedPaths); },
      Qt::QueuedConnection);
}

void notifyAndroidMultiSelectionCancelled() {
  QObject* application = QCoreApplication::instance();
  if (application == nullptr) {
    completeMultiSelectionOnQtThread(QStringList());
    return;
  }

  QMetaObject::invokeMethod(
      application, []() { completeMultiSelectionOnQtThread(QStringList()); },
      Qt::QueuedConnection);
}
#endif

}  // namespace

#ifdef Q_OS_ANDROID
extern "C" JNIEXPORT void JNICALL
Java_org_patronus_client_PatronusActivity_nativeOnFilePicked(
    JNIEnv* env, jclass, jstring fileName, jstring filePath) {
  const QString pickedFileName =
      fileName != nullptr ? QJniObject(fileName).toString() : QString();
  const QString pickedFilePath =
      filePath != nullptr ? QJniObject(filePath).toString() : QString();

  completeSelectionFromAndroid(pickedFileName, pickedFilePath);
}

extern "C" JNIEXPORT void JNICALL
Java_org_patronus_client_PatronusActivity_nativeOnFilePickCancelled(JNIEnv*,
                                                                  jclass) {
  notifyAndroidSelectionCancelled();
}

extern "C" JNIEXPORT void JNICALL
Java_org_patronus_client_PatronusActivity_nativeOnFilesPicked(
    JNIEnv* env, jclass, jobjectArray fileNames, jobjectArray filePaths) {
  QStringList names;
  QStringList paths;

  if (fileNames != nullptr && filePaths != nullptr) {
    const jsize count = env->GetArrayLength(fileNames);
    for (jsize i = 0; i < count; ++i) {
      jstring nameObj =
          static_cast<jstring>(env->GetObjectArrayElement(fileNames, i));
      names.append(nameObj != nullptr ? QJniObject(nameObj).toString()
                                      : QString());
      if (nameObj != nullptr) {
        env->DeleteLocalRef(nameObj);
      }

      jstring pathObj =
          static_cast<jstring>(env->GetObjectArrayElement(filePaths, i));
      paths.append(pathObj != nullptr ? QJniObject(pathObj).toString()
                                      : QString());
      if (pathObj != nullptr) {
        env->DeleteLocalRef(pathObj);
      }
    }
  }

  completeMultiSelectionFromAndroid(names, paths);
}

extern "C" JNIEXPORT void JNICALL
Java_org_patronus_client_PatronusActivity_nativeOnFilesPickCancelled(JNIEnv*,
                                                                   jclass) {
  notifyAndroidMultiSelectionCancelled();
}
#endif

void pickOpenFile(QObject* context, const QString& title,
                  const QString& initialDirectory, const QString& filter,
                  SelectionCallback callback) {
  if (!callback) {
    return;
  }

#ifdef Q_OS_ANDROID
  Q_UNUSED(title);
  Q_UNUSED(initialDirectory);

  {
    QMutexLocker locker(&g_callbackMutex);
    g_callbackContext = context;
    g_selectionCallback = std::move(callback);
  }

  QObject* application = QCoreApplication::instance();
  if (application == nullptr) {
    notifyAndroidSelectionCancelled();
    return;
  }

  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    notifyAndroidSelectionCancelled();
    return;
  }

  const QJniObject filterString = QJniObject::fromString(filter);
  activity.callMethod<void>("openFilePicker", "(Ljava/lang/String;)V",
                            filterString.object<jstring>());

  QJniEnvironment env;
  if (env.checkAndClearExceptions()) {
    notifyAndroidSelectionCancelled();
  }
#else
  const QString filePath =
      QFileDialog::getOpenFileName(nullptr, title, initialDirectory, filter);
  callback(filePath);
#endif
}

void pickOpenFiles(QObject* context, const QString& title,
                   const QString& initialDirectory, const QString& filter,
                   MultiSelectionCallback callback) {
  if (!callback) {
    return;
  }

#ifdef Q_OS_ANDROID
  Q_UNUSED(title);
  Q_UNUSED(initialDirectory);

  {
    QMutexLocker locker(&g_callbackMutex);
    g_multiCallbackContext = context;
    g_multiSelectionCallback = std::move(callback);
  }

  QObject* application = QCoreApplication::instance();
  if (application == nullptr) {
    notifyAndroidMultiSelectionCancelled();
    return;
  }

  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    notifyAndroidMultiSelectionCancelled();
    return;
  }

  const QJniObject filterString = QJniObject::fromString(filter);
  activity.callMethod<void>("openFilesPicker", "(Ljava/lang/String;)V",
                            filterString.object<jstring>());

  QJniEnvironment env;
  if (env.checkAndClearExceptions()) {
    notifyAndroidMultiSelectionCancelled();
  }
#else
  const QStringList filePaths = QFileDialog::getOpenFileNames(
      nullptr, title, initialDirectory, filter);
  callback(filePaths);
#endif
}

}  // namespace filepicker