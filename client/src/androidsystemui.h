#pragma once

#include <QColor>
#include <QObject>
#include <QString>

class AndroidSystemUi : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool available READ isAvailable CONSTANT)
  Q_PROPERTY(
      int keyboardHeight READ keyboardHeight NOTIFY keyboardHeightChanged)
  Q_PROPERTY(
      bool speakerphoneOn READ speakerphoneOn NOTIFY speakerphoneOnChanged)
  Q_PROPERTY(
      bool callAudioActive READ callAudioActive NOTIFY callAudioActiveChanged)
  // True when the platform draws our window edge-to-edge under the system bars
  // (Android 15+/SDK>=35, where edge-to-edge is enforced). On older Android the
  // system already insets our content below the status bar AND resizes the
  // window for the soft keyboard, so QML must NOT add its own manual insets.
  Q_PROPERTY(bool edgeToEdge READ edgeToEdge CONSTANT)

 public:
  explicit AndroidSystemUi(QObject* parent = nullptr);

  static AndroidSystemUi* instance();

  bool isAvailable() const;
  bool edgeToEdge() const;
  int keyboardHeight() const;
  bool speakerphoneOn() const;
  bool callAudioActive() const;

  Q_INVOKABLE void setStatusBarColor(const QColor& color, bool darkIcons);
  Q_INVOKABLE void setStatusBarColor(const QColor& color);
  Q_INVOKABLE void invalidateInput();
  Q_INVOKABLE void restartInput();
  Q_INVOKABLE void showKeyboard();
  Q_INVOKABLE void showKeyboardWithHints(int qtImhHints);
  Q_INVOKABLE void setCallAudioActive(bool active);
  Q_INVOKABLE void setCallProximityActive(bool active);
  Q_INVOKABLE void setSpeakerphoneOn(bool enabled);
  Q_INVOKABLE void toggleSpeakerphone();
  Q_INVOKABLE void startRingtone(const QString& resourcePath, bool useEarpiece = false);
  Q_INVOKABLE void stopRingtone();
  Q_INVOKABLE void startCallVibration(int patternIndex);
  Q_INVOKABLE void stopCallVibration();
  Q_INVOKABLE QString saveFileToDownloads(const QString& sourcePath,
                                          const QString& displayName);
  Q_INVOKABLE bool installApk(const QString& apkPath);
  // Returns a writable path (under the app temp dir) suitable for saving a file
  // that will then be shared. Works on every platform.
  Q_INVOKABLE QString shareableFilePath(const QString& fileName);
  // Opens the Android share sheet for the given file. Returns false on
  // non-Android platforms or on failure (the caller should fall back).
  Q_INVOKABLE bool shareImage(const QString& path, const QString& text);
  Q_INVOKABLE void showSystemNotification(const QString& title,
                                          const QString& message);
  // Relabels the FCM-posted incoming-call notification with the resolved caller
  // name once the call_request is decrypted; no-op when no such notification is
  // showing (e.g. a live in-app call).
  Q_INVOKABLE void updateIncomingCallNotification(const QString& callerName);
  // Dismisses the incoming-call notification when the call is shown/answered/ended.
  Q_INVOKABLE void cancelIncomingCallNotification();
  // Sweeps away the message notifications piled up in the tray (any live
  // incoming-call notification is preserved). Called when the user opens a chat
  // so the notification does not linger after the messages have been read.
  Q_INVOKABLE void clearMessageNotifications();
  // Returns and clears the Accept/Decline action the user picked from the call
  // notification (or empty if none), so the call UI can auto-perform it.
  Q_INVOKABLE QString consumePendingCallAction();
  // True while an FCM incoming-call notification is live, so the call UI can
  // suppress its own ringtone (the notification ringer is already ringing).
  Q_INVOKABLE bool hasActiveIncomingCallNotification();
  // Tells the activity the call was answered, so a later end-of-call does not
  // send it to the background.
  Q_INVOKABLE void markCallAnswered();
  // When an FCM-launched call ends unanswered, returns the device to its locked/
  // asleep state instead of leaving the app on screen.
  Q_INVOKABLE void dismissCallScreenIfLaunchedForCall();
  Q_INVOKABLE void initializeFcmForServer(const QString& serverKey,
                                          const QString& clientConfigJson);
  Q_INVOKABLE void requestFcmTokenForServer(const QString& serverKey);
  Q_INVOKABLE QString getCachedFcmTokenForServer(const QString& serverKey);
  Q_INVOKABLE void clearFcmServerState(const QString& serverKey);

  static void setKeyboardHeightFromJni(int heightPixels);
  static void setSpeakerphoneOnFromJni(bool enabled);
  static void emitFcmTokenReadyFromJni(const QString& serverKey,
                                       const QString& token);
  static void emitFcmTokenErrorFromJni(const QString& serverKey,
                                       const QString& errorText);
  // Poked from JNI when the user taps Accept/Decline on the call notification, so
  // a call already on screen applies it immediately (see CallManager).
  static void emitCallActionFromJni();

 signals:
  void keyboardHeightChanged();
  void speakerphoneOnChanged();
  void callAudioActiveChanged();
  void fcmTokenReady(const QString& serverKey, const QString& token);
  void fcmTokenError(const QString& serverKey, const QString& errorText);
  void notificationCallActionRequested();

 private:
  static AndroidSystemUi* s_instance;
  bool m_edgeToEdge = false;
  int m_keyboardHeight = 0;
  int m_lastNonZeroKeyboardHeight = 0;
  qreal m_devicePixelRatio = 1.0;
  bool m_speakerphoneOn = false;
  bool m_callAudioActive = false;
  bool m_callProximityActive = false;
};