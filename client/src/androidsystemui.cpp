#include "androidsystemui.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>

#include "apppaths.h"

#ifdef Q_OS_ANDROID
#include <QtCore/qcoreapplication_platform.h>

#include <QJniEnvironment>
#include <QJniObject>
#endif

AndroidSystemUi* AndroidSystemUi::s_instance = nullptr;

AndroidSystemUi::AndroidSystemUi(QObject* parent) : QObject(parent) {
  s_instance = this;
  if (auto* screen = QGuiApplication::primaryScreen()) {
    m_devicePixelRatio = screen->devicePixelRatio();
  }
#ifdef Q_OS_ANDROID
  // Mirror Qt's QtWindowInsetsController.edgeToEdgeEnabled(): edge-to-edge is
  // enforced on Android 15+ (SDK 35, VANILLA_ICE_CREAM). We don't opt out via
  // the theme, so SDK>=35 => edge-to-edge. On older releases Qt keeps the window
  // non-edge-to-edge (content pre-inset below the status bar, window resized for
  // the keyboard), and the QML must not add its own status-bar/keyboard insets.
  const int sdkInt =
      QJniObject::getStaticField<jint>("android/os/Build$VERSION", "SDK_INT");
  m_edgeToEdge = sdkInt >= 35;
#endif
}

AndroidSystemUi* AndroidSystemUi::instance() { return s_instance; }

bool AndroidSystemUi::edgeToEdge() const { return m_edgeToEdge; }

int AndroidSystemUi::keyboardHeight() const { return m_keyboardHeight; }

bool AndroidSystemUi::speakerphoneOn() const { return m_speakerphoneOn; }

bool AndroidSystemUi::callAudioActive() const { return m_callAudioActive; }

void AndroidSystemUi::setKeyboardHeightFromJni(int heightPixels) {
  if (!s_instance) return;

  int logicalHeight =
      qRound(static_cast<qreal>(heightPixels) / s_instance->m_devicePixelRatio);

  QMetaObject::invokeMethod(
      s_instance,
      [logicalHeight]() {
        if (!s_instance) return;
        if (logicalHeight > 0)
          s_instance->m_lastNonZeroKeyboardHeight = logicalHeight;
        if (s_instance->m_keyboardHeight == logicalHeight) return;
        s_instance->m_keyboardHeight = logicalHeight;
        emit s_instance->keyboardHeightChanged();
      },
      Qt::QueuedConnection);
}

void AndroidSystemUi::setSpeakerphoneOnFromJni(bool enabled) {
  if (!s_instance) {
    return;
  }

  QMetaObject::invokeMethod(
      s_instance,
      [enabled]() {
        if (!s_instance || s_instance->m_speakerphoneOn == enabled) {
          return;
        }

        s_instance->m_speakerphoneOn = enabled;
        emit s_instance->speakerphoneOnChanged();
      },
      Qt::QueuedConnection);
}

void AndroidSystemUi::emitFcmTokenReadyFromJni(const QString& serverKey,
                                               const QString& token) {
  if (!s_instance) {
    return;
  }

  qDebug() << "[AndroidSystemUi] JNI FCM token ready" << serverKey
           << "tokenLen=" << token.size();

  QMetaObject::invokeMethod(
      s_instance,
      [serverKey, token]() {
        if (!s_instance) {
          return;
        }

        emit s_instance->fcmTokenReady(serverKey, token);
      },
      Qt::QueuedConnection);
}

void AndroidSystemUi::emitFcmTokenErrorFromJni(const QString& serverKey,
                                               const QString& errorText) {
  if (!s_instance) {
    return;
  }

  qWarning() << "[AndroidSystemUi] JNI FCM token error" << serverKey
             << errorText;

  QMetaObject::invokeMethod(
      s_instance,
      [serverKey, errorText]() {
        if (!s_instance) {
          return;
        }

        emit s_instance->fcmTokenError(serverKey, errorText);
      },
      Qt::QueuedConnection);
}

void AndroidSystemUi::emitCallActionFromJni() {
  if (!s_instance) {
    return;
  }

  QMetaObject::invokeMethod(
      s_instance,
      []() {
        if (!s_instance) {
          return;
        }

        emit s_instance->notificationCallActionRequested();
      },
      Qt::QueuedConnection);
}

#ifdef Q_OS_ANDROID
extern "C" JNIEXPORT void JNICALL
Java_org_patronus_client_PatronusActivity_nativeOnKeyboardHeightChanged(
    JNIEnv*, jclass, jint heightPixels) {
  AndroidSystemUi::setKeyboardHeightFromJni(static_cast<int>(heightPixels));
}

extern "C" JNIEXPORT void JNICALL
Java_org_patronus_client_PatronusActivity_nativeOnCallActionFromNotification(
    JNIEnv*, jclass) {
  AndroidSystemUi::emitCallActionFromJni();
}

extern "C" JNIEXPORT void JNICALL
Java_org_patronus_client_PatronusActivity_nativeOnSpeakerphoneChanged(
    JNIEnv*, jclass, jboolean enabled) {
  AndroidSystemUi::setSpeakerphoneOnFromJni(enabled);
}

extern "C" JNIEXPORT void JNICALL
Java_org_patronus_client_PatronusActivity_nativeOnFcmTokenReady(JNIEnv* env,
                                                              jclass,
                                                              jstring serverKey,
                                                              jstring token) {
  const QString qServerKey =
      serverKey != nullptr ? QJniObject(serverKey).toString() : QString();
  const QString qToken =
      token != nullptr ? QJniObject(token).toString() : QString();
  AndroidSystemUi::emitFcmTokenReadyFromJni(qServerKey, qToken);
  Q_UNUSED(env);
}

extern "C" JNIEXPORT void JNICALL
Java_org_patronus_client_PatronusActivity_nativeOnFcmTokenError(
    JNIEnv* env, jclass, jstring serverKey, jstring errorText) {
  const QString qServerKey =
      serverKey != nullptr ? QJniObject(serverKey).toString() : QString();
  const QString qErrorText =
      errorText != nullptr ? QJniObject(errorText).toString() : QString();
  AndroidSystemUi::emitFcmTokenErrorFromJni(qServerKey, qErrorText);
  Q_UNUSED(env);
}
#endif

bool AndroidSystemUi::isAvailable() const {
#ifdef Q_OS_ANDROID
  return true;
#else
  return false;
#endif
}

void AndroidSystemUi::invalidateInput() {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return;
  }
  activity.callMethod<void>("invalidateInput", "()V");
  QJniEnvironment env;
  env.checkAndClearExceptions();
#endif
}

void AndroidSystemUi::restartInput() {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return;
  }
  activity.callMethod<void>("restartInput", "()V");
  QJniEnvironment env;
  env.checkAndClearExceptions();
#endif
}

void AndroidSystemUi::showKeyboard() {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return;
  }
  activity.callMethod<void>("showSoftKeyboard", "()V");
  QJniEnvironment env;
  env.checkAndClearExceptions();
#endif
}

void AndroidSystemUi::showKeyboardWithHints(int qtImhHints) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return;
  }
  activity.callMethod<void>("showSoftKeyboardWithHints", "(I)V",
                            static_cast<jint>(qtImhHints));
  QJniEnvironment env;
  env.checkAndClearExceptions();
#else
  Q_UNUSED(qtImhHints);
#endif
}

void AndroidSystemUi::setCallAudioActive(bool active) {
  if (m_callAudioActive == active) {
    return;
  }

  m_callAudioActive = active;
  emit callAudioActiveChanged();

#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (activity.isValid()) {
    activity.callMethod<void>("setCallAudioActive", "(Z)V",
                              static_cast<jboolean>(active));
    QJniEnvironment env;
    env.checkAndClearExceptions();
  }
#else
  Q_UNUSED(active);
#endif

  if (!active && m_speakerphoneOn) {
    m_speakerphoneOn = false;
    emit speakerphoneOnChanged();
  }
}

void AndroidSystemUi::setCallProximityActive(bool active) {
  if (m_callProximityActive == active) {
    return;
  }

  m_callProximityActive = active;

#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (activity.isValid()) {
    activity.callMethod<void>("setCallProximityActive", "(Z)V",
                              static_cast<jboolean>(active));
    QJniEnvironment env;
    env.checkAndClearExceptions();
  }
#else
  Q_UNUSED(active);
#endif
}

void AndroidSystemUi::setSpeakerphoneOn(bool enabled) {
  if (m_speakerphoneOn == enabled && !m_callAudioActive) {
    return;
  }

#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (activity.isValid()) {
    activity.callMethod<void>("setSpeakerphoneEnabled", "(Z)V",
                              static_cast<jboolean>(enabled));
    QJniEnvironment env;
    env.checkAndClearExceptions();
  }
#endif

  if (m_speakerphoneOn == enabled) {
    return;
  }

  m_speakerphoneOn = enabled;
  emit speakerphoneOnChanged();
}

void AndroidSystemUi::toggleSpeakerphone() {
  setSpeakerphoneOn(!m_speakerphoneOn);
}

QString AndroidSystemUi::saveFileToDownloads(const QString& sourcePath,
                                             const QString& displayName) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return QString();
  }

  const QJniObject jSourcePath = QJniObject::fromString(sourcePath);
  const QJniObject jDisplayName = QJniObject::fromString(displayName);
  const QJniObject result = activity.callObjectMethod(
      "saveFileToDownloads",
      "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
      jSourcePath.object<jstring>(), jDisplayName.object<jstring>());

  QJniEnvironment env;
  env.checkAndClearExceptions();
  return result.toString();
#else
  Q_UNUSED(sourcePath);
  Q_UNUSED(displayName);
  return QString();
#endif
}

bool AndroidSystemUi::installApk(const QString& apkPath) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return false;
  }

  const QJniObject jApkPath = QJniObject::fromString(apkPath);
  const bool result = activity.callMethod<jboolean>(
      "installApk", "(Ljava/lang/String;)Z", jApkPath.object<jstring>());

  QJniEnvironment env;
  env.checkAndClearExceptions();
  return result;
#else
  Q_UNUSED(apkPath);
  return false;
#endif
}

QString AndroidSystemUi::shareableFilePath(const QString& fileName) {
  const QString dir = apppaths::tempDirectory();
  if (dir.isEmpty() || fileName.isEmpty()) {
    return QString();
  }
  return QDir(dir).filePath(fileName);
}

bool AndroidSystemUi::shareImage(const QString& path, const QString& text) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return false;
  }

  const QJniObject jPath = QJniObject::fromString(path);
  const QJniObject jText = QJniObject::fromString(text);
  const bool result = activity.callMethod<jboolean>(
      "shareImage", "(Ljava/lang/String;Ljava/lang/String;)Z",
      jPath.object<jstring>(), jText.object<jstring>());

  QJniEnvironment env;
  env.checkAndClearExceptions();
  return result;
#else
  Q_UNUSED(path);
  Q_UNUSED(text);
  return false;
#endif
}

void AndroidSystemUi::showSystemNotification(const QString& title,
                                             const QString& message) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return;
  }

  const QJniObject jTitle = QJniObject::fromString(title);
  const QJniObject jMessage = QJniObject::fromString(message);
  activity.callMethod<void>(
      "showSystemNotification",
      "(Ljava/lang/String;Ljava/lang/String;)V",
      jTitle.object<jstring>(), jMessage.object<jstring>());

  QJniEnvironment env;
  env.checkAndClearExceptions();
#else
  Q_UNUSED(title);
  Q_UNUSED(message);
#endif
}

void AndroidSystemUi::updateIncomingCallNotification(const QString& callerName) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return;
  }

  const QJniObject jCallerName = QJniObject::fromString(callerName);
  activity.callMethod<void>("updateIncomingCallNotification",
                            "(Ljava/lang/String;)V",
                            jCallerName.object<jstring>());

  QJniEnvironment env;
  env.checkAndClearExceptions();
#else
  Q_UNUSED(callerName);
#endif
}

void AndroidSystemUi::cancelIncomingCallNotification() {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return;
  }

  activity.callMethod<void>("cancelIncomingCallNotification", "()V");

  QJniEnvironment env;
  env.checkAndClearExceptions();
#endif
}

void AndroidSystemUi::clearMessageNotifications() {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return;
  }

  activity.callMethod<void>("clearMessageNotifications", "()V");

  QJniEnvironment env;
  env.checkAndClearExceptions();
#endif
}

QString AndroidSystemUi::consumePendingCallAction() {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return QString();
  }

  const QJniObject result = activity.callObjectMethod(
      "consumePendingCallAction", "()Ljava/lang/String;");

  QJniEnvironment env;
  env.checkAndClearExceptions();
  return result.isValid() ? result.toString() : QString();
#else
  return QString();
#endif
}

bool AndroidSystemUi::hasActiveIncomingCallNotification() {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return false;
  }

  const bool result =
      activity.callMethod<jboolean>("isIncomingCallNotificationActive", "()Z");

  QJniEnvironment env;
  env.checkAndClearExceptions();
  return result;
#else
  return false;
#endif
}

void AndroidSystemUi::markCallAnswered() {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return;
  }

  activity.callMethod<void>("markCallAnswered", "()V");

  QJniEnvironment env;
  env.checkAndClearExceptions();
#endif
}

void AndroidSystemUi::dismissCallScreenIfLaunchedForCall() {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return;
  }

  activity.callMethod<void>("dismissCallScreenIfLaunchedForCall", "()V");

  QJniEnvironment env;
  env.checkAndClearExceptions();
#endif
}

void AndroidSystemUi::initializeFcmForServer(const QString& serverKey,
                                             const QString& clientConfigJson) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    qWarning() << "[AndroidSystemUi] initializeFcmForServer called without "
                  "valid activity for"
               << serverKey;
    return;
  }

  qDebug() << "[AndroidSystemUi] initializeFcmForServer" << serverKey
           << "configBytes=" << clientConfigJson.toUtf8().size();

  const QJniObject jServerKey = QJniObject::fromString(serverKey);
  const QJniObject jClientConfigJson = QJniObject::fromString(clientConfigJson);
  activity.callMethod<void>(
      "initializeFcmForServer", "(Ljava/lang/String;Ljava/lang/String;)V",
      jServerKey.object<jstring>(), jClientConfigJson.object<jstring>());

  QJniEnvironment env;
  env.checkAndClearExceptions();
#else
  Q_UNUSED(serverKey);
  Q_UNUSED(clientConfigJson);
#endif
}

void AndroidSystemUi::requestFcmTokenForServer(const QString& serverKey) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    qWarning() << "[AndroidSystemUi] requestFcmTokenForServer called without "
                  "valid activity for"
               << serverKey;
    return;
  }

  qDebug() << "[AndroidSystemUi] requestFcmTokenForServer" << serverKey;

  const QJniObject jServerKey = QJniObject::fromString(serverKey);
  activity.callMethod<void>("requestFcmTokenForServer", "(Ljava/lang/String;)V",
                            jServerKey.object<jstring>());

  QJniEnvironment env;
  env.checkAndClearExceptions();
#else
  Q_UNUSED(serverKey);
#endif
}

QString AndroidSystemUi::getCachedFcmTokenForServer(const QString& serverKey) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    qWarning() << "[AndroidSystemUi] getCachedFcmTokenForServer called without "
                  "valid activity for"
               << serverKey;
    return QString();
  }

  const QJniObject jServerKey = QJniObject::fromString(serverKey);
  const QJniObject result = activity.callObjectMethod(
      "getCachedFcmTokenForServer", "(Ljava/lang/String;)Ljava/lang/String;",
      jServerKey.object<jstring>());

  QJniEnvironment env;
  env.checkAndClearExceptions();
  const QString token = result.toString();
  qDebug() << "[AndroidSystemUi] getCachedFcmTokenForServer" << serverKey
           << "tokenLen=" << token.size();
  return token;
#else
  Q_UNUSED(serverKey);
  return QString();
#endif
}

void AndroidSystemUi::clearFcmServerState(const QString& serverKey) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    qWarning() << "[AndroidSystemUi] clearFcmServerState called without valid "
                  "activity for"
               << serverKey;
    return;
  }

  qDebug() << "[AndroidSystemUi] clearFcmServerState" << serverKey;

  const QJniObject jServerKey = QJniObject::fromString(serverKey);
  activity.callMethod<void>("clearFcmServerState", "(Ljava/lang/String;)V",
                            jServerKey.object<jstring>());

  QJniEnvironment env;
  env.checkAndClearExceptions();
#else
  Q_UNUSED(serverKey);
#endif
}

void AndroidSystemUi::setStatusBarColor(const QColor& color, bool darkIcons) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) {
    return;
  }

  activity.callMethod<void>("applySystemBarStyle", "(IZ)V",
                            static_cast<jint>(color.rgba()),
                            static_cast<jboolean>(darkIcons));

  QJniEnvironment env;
  env.checkAndClearExceptions();
#else
  Q_UNUSED(color);
  Q_UNUSED(darkIcons);
#endif
}

void AndroidSystemUi::setStatusBarColor(const QColor& color) {
  setStatusBarColor(color, false);
}

void AndroidSystemUi::startRingtone(const QString& resourcePath, bool useEarpiece) {
#ifdef Q_OS_ANDROID
  QFile src(resourcePath);
  if (!src.open(QIODevice::ReadOnly)) {
    qWarning() << "[AndroidSystemUi] startRingtone: cannot open" << resourcePath;
    return;
  }
  const QString tempPath = QDir::tempPath() + QStringLiteral("/patronus_ring.wav");
  {
    QFile dst(tempPath);
    if (!dst.open(QIODevice::WriteOnly)) {
      qWarning() << "[AndroidSystemUi] startRingtone: cannot write" << tempPath;
      return;
    }
    dst.write(src.readAll());
  }
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) return;
  const QJniObject jPath = QJniObject::fromString(tempPath);
  activity.callMethod<void>("startRingtone", "(Ljava/lang/String;Z)V",
                            jPath.object<jstring>(), (jboolean)useEarpiece);
  QJniEnvironment env;
  env.checkAndClearExceptions();
#else
  Q_UNUSED(resourcePath);
  Q_UNUSED(useEarpiece);
#endif
}

void AndroidSystemUi::stopRingtone() {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) return;
  activity.callMethod<void>("stopRingtone", "()V");
  QJniEnvironment env;
  env.checkAndClearExceptions();
#endif
}

void AndroidSystemUi::startCallVibration(int patternIndex) {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) return;
  activity.callMethod<void>("startCallVibration", "(I)V",
                            static_cast<jint>(patternIndex));
  QJniEnvironment env;
  env.checkAndClearExceptions();
#else
  Q_UNUSED(patternIndex);
#endif
}

void AndroidSystemUi::stopCallVibration() {
#ifdef Q_OS_ANDROID
  const QJniObject activity = QNativeInterface::QAndroidApplication::context();
  if (!activity.isValid()) return;
  activity.callMethod<void>("stopCallVibration", "()V");
  QJniEnvironment env;
  env.checkAndClearExceptions();
#endif
}

