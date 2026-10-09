#pragma once

#include <QList>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>

#include "connectionmanager.h"
#include "corecrypto.h"
#include "opusplayer.h"
#include "opusrecorder.h"

class AudioDeviceManager;
class DatabaseManager;
class MessageModel;
class ContactsModel;
class QScreen;

constexpr size_t CHACHAPOLY1305_NONCE_SIZE = 12;
constexpr size_t AUDIO_FRAME_NONCE_SIZE = 8;
constexpr int GITTER_DEFAULT_BUFFER_MS = 150;

class CallManager : public QObject {
  Q_OBJECT
  Q_PROPERTY(int jitterBufferMs READ jitterBufferMs WRITE setJitterBufferMs
                 NOTIFY jitterBufferMsChanged)
  Q_PROPERTY(qreal remoteVoiceLevel READ remoteVoiceLevel NOTIFY
                 remoteVoiceLevelChanged)
 public:
  explicit CallManager(QQmlApplicationEngine* engine, QObject* parent = nullptr,
                       ConnectionManager* connectionManager = nullptr,
                       DatabaseManager* db = nullptr,
                       MessageModel* messageModel = nullptr,
                       ContactsModel* contactsModel = nullptr,
                       AudioDeviceManager* audioDeviceManager = nullptr);

  ~CallManager();

  Q_INVOKABLE void startCall(const QString& contactFirstName,
                             const QString& contactAvatar,
                             const QString& contactPubKey,
                             const QString& contactServer);

  Q_INVOKABLE void showCallError(const QString& contactFirstName,
                                 const QString& contactAvatar,
                                 const QString& contactPubKey,
                                 const QString& contactServer,
                                 const QString& errorMessage);

  Q_INVOKABLE void showCallRequest(const QString& contactFirstName,
                                   const QString& contactAvatar,
                                   const QString& contactServer,
                                   const QString& contactPubKey,
                                   const QString& dh_pub, const QString& id,
                                   const uint64_t callId);

  Q_INVOKABLE void discard(const QString& contactServer,
                           const QString& contactPubKey, const uint64_t callId,
                           const bool isOwnCallRequest, const QString& type);

  Q_INVOKABLE void cancel(const QString& contactServer,
                          const QString& contactPubKey, const uint64_t callId,
                          const bool isOwnCallRequest, const QString& type);

  Q_INVOKABLE void accept(const QString& contactServer,
                          const QString& contactPubKey, const QString& dh_pub,
                          const QString& id, const uint64_t callId);

  Q_INVOKABLE void startAudioStream();
  Q_INVOKABLE void stopAudioStream();
  Q_INVOKABLE void setJitterBufferMs(int ms);
  Q_INVOKABLE void dismissCallUi();
  void refreshAudioOutputRoute();
  void setPreferredDesktopScreen(QScreen* screen);

  int jitterBufferMs() const { return m_jitterBufferMs; }
  void onAudioFrameReceived(const QByteArray& encryptedFrame);
  void onCallAccepted();
  qreal remoteVoiceLevel() const { return m_remoteVoiceLevel; }

 signals:
  void jitterBufferMsChanged();
  void audioError(const QString& error);
  void remoteVoiceLevelChanged(qreal level);

 private slots:
  void onAudioFrameEncoded(const QByteArray& opusData);
  void onRecorderError(const QString& error);
  void onPlayerError(const QString& error);
  void onRemoteVoiceLevelChanged(qreal level);
    void onCallDiscardReceived(const QString& serverId,
                               const QString& contactPubKey,
                               uint64_t callId, const QString& reason);
  void onCallCancelReceived(const QString& serverId,
                            const QString& contactPubKey, uint64_t callId);
  void onConnectionAvailabilityChanged(const QString& serverId, bool available);
  void onOfflineSyncStarted(const QString& serverId);
  void onOfflineSyncEnded(const QString& serverId);

 private:
  struct PendingIncomingCall;
  QObject* createCallUi();
  void destroyCallUi();
  void presentCallRequest(const PendingIncomingCall& call);
  // Applies an Accept/Decline the user picked from the call notification, if a
  // call is on screen. Safe to call repeatedly: consumePendingCallAction() is
  // the single gate, so the action is applied exactly once whether it arrives
  // before the call is shown (handled in presentCallRequest) or after (handled
  // via the notification poke).
  void applyPendingCallAction();
  void addCallLogMessage(const QString& contactServer,
                         const QString& contactPubKey, quint64 callId,
                         const QString& text, bool isOwnCallRequest);
  bool useEmbeddedCallUi() const;
  QQuickWindow* mainApplicationWindow() const;
  QString activeCallServerId() const;
  void resetCallState(bool closeUi);
  void setCurrentCallIdentity(const QString& contactServer,
                              const QString& contactPubKey,
                              quint64 callId);

  DatabaseManager* m_db = nullptr;
  MessageModel* m_messageModel = nullptr;
  ContactsModel* m_contactsModel = nullptr;
  void sendAudioFrame(const QByteArray& opusData);
  QByteArray encryptAudioFrame(const QByteArray& plainFrame);
  QByteArray decryptAudioFrame(const QByteArray& encryptedFrame);

  ConnectionManager* m_connectionManager;
  QQmlApplicationEngine* m_engine;
  QObject* m_callUi = nullptr;
  CoreCrypto::SessionKeys m_sessionKeys;

  OpusRecorder* m_recorder = nullptr;
  OpusPlayer* m_player = nullptr;

  uint64_t m_audioSendCounter = 0;
  uint64_t m_audioRecvCounter = 0;

  bool m_audioActive = false;
  int m_jitterBufferMs = GITTER_DEFAULT_BUFFER_MS;

  qreal m_remoteVoiceLevel = 0.0;
  QPointer<QScreen> m_preferredDesktopScreen;

  QString m_currentContactServer;
  QString m_currentContactPubKey;
  quint64 m_currentCallId = 0;
  // True while the CURRENT call still owns a live OS-level incoming-call
  // notification/ringer (set from presentCallRequest's externalRing check,
  // cleared once dismissed). resetCallState() only cancels the OS
  // notification when this is true, so ending a call that never had one (a
  // purely in-app call, or one whose notification was already dismissed at
  // accept time) cannot stomp on a different, still-ringing call's
  // notification — see NotificationHelper's single global notification slot.
  bool m_incomingNotificationPending = false;
  // Epoch seconds when the current call became active (accepted); 0 when no
  // call is active. Used to compute and persist the call's duration once it
  // ends, in resetCallState().
  quint64 m_activeCallStartEpoch = 0;

  struct PendingIncomingCall {
    QString firstName;
    QString avatarSource;
    QString contactServer;
    QString contactPubKey;
    QString dh_pub;
    QString id;
    quint64 callId = 0;
  };
  // Incoming calls received while replaying the offline message queue are held
  // here instead of being shown immediately, so that a call_cancel sitting right
  // behind the call_request in the same queue can suppress the ring. They are
  // shown (or dropped) once the sync for their server finishes. Live calls are
  // never buffered and ring instantly.
  QSet<QString> m_syncingServers;
  QList<PendingIncomingCall> m_bufferedIncomingCalls;
};
