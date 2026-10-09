#pragma once

#include <QObject>
#include <QVariantList>

#include "voiceclipplayer.h"
#include "voicecliprecorder.h"

class FileTransferManager;
class AudioDeviceManager;

class VoiceMessageManager : public QObject {
  Q_OBJECT

  Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
  Q_PROPERTY(int recordingDurationMs READ recordingDurationMs NOTIFY
                 recordingDurationChanged)
  Q_PROPERTY(QVariantList recordingLevelBars READ recordingLevelBars NOTIFY
                 recordingLevelBarsChanged)
  Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)
  Q_PROPERTY(
      bool playbackPaused READ isPlaybackPaused NOTIFY playingStateChanged)
  Q_PROPERTY(
      quint64 playingMessageId READ playingMessageId NOTIFY playingStateChanged)
  Q_PROPERTY(
      int playbackPositionMs READ playbackPositionMs NOTIFY playingStateChanged)
  Q_PROPERTY(
      int playbackDurationMs READ playbackDurationMs NOTIFY playingStateChanged)

 public:
  explicit VoiceMessageManager(FileTransferManager* fileTransferManager,
                               AudioDeviceManager* audioDeviceManager,
                               QObject* parent = nullptr);

  bool isRecording() const;
  int recordingDurationMs() const;
  QVariantList recordingLevelBars() const;
  bool isPlaying() const;
  bool isPlaybackPaused() const;
  quint64 playingMessageId() const;
  int playbackPositionMs() const;
  int playbackDurationMs() const;

  Q_INVOKABLE void startRecording();
  Q_INVOKABLE void cancelRecording();
  Q_INVOKABLE void stopRecordingAndSend(
      const QString& serverId, const QString& contactPubKey,
      quint64 replyTo = 0, const QString& replyPreview = QString(),
      const QString& replyKind = QString(), bool replyIsOwn = false);
  Q_INVOKABLE void togglePlayback(quint64 messageId, const QString& localPath);
  Q_INVOKABLE void seekPlayback(quint64 messageId, const QString& localPath,
                                qreal positionRatio);
  Q_INVOKABLE void stopPlayback();

 signals:
  void recordingChanged();
  void recordingDurationChanged();
  void recordingLevelBarsChanged();
  void playingChanged();
  void playingStateChanged();
  void errorOccurred(const QString& errorText);

 private slots:
  void onFrameEncoded(const QByteArray& opusFrame);
  void onFrameLevelCaptured(qreal level);
  void onRecorderError(const QString& errorText);
  void onPlayerError(const QString& errorText);
  void onPlayerPositionChanged(int positionMs);

 private:
  QList<int> buildWaveform() const;
  void updateRecordingLevelBars();
  void resetRecordingState();
  QString buildRecordingPath() const;
  bool loadPlaybackClip(quint64 messageId, const QString& localPath);
  void pausePlayback();
  void resumePlayback();
  void finishPlayback();

  FileTransferManager* m_fileTransferManager = nullptr;
  VoiceClipRecorder m_recorder;
  VoiceClipPlayer m_player;
  QList<QByteArray> m_recordedFrames;
  QList<qreal> m_recordedFrameLevels;
  QList<qreal> m_recentRecordingLevels;
  QList<int> m_recordingLevelBars;
  qreal m_recordingLevelReference = 0.12;
  int m_recordingDurationMs = 0;
  int m_recordingUiFrameCounter = 0;
  quint64 m_playingMessageId = 0;
  bool m_playbackPaused = false;
  int m_playbackPositionMs = 0;
  int m_playbackDurationMs = 0;
};