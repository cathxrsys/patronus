#pragma once

#include <opus/opus.h>

#include <QAudioSink>
#include <QBuffer>
#include <QObject>
#include <QTimer>

class VoiceClipPlayer : public QObject {
  Q_OBJECT

 public:
  explicit VoiceClipPlayer(QObject* parent = nullptr);
  ~VoiceClipPlayer() override;

  bool loadClip(const QString& localPath, QString* errorText = nullptr);
  void play();
  void pause();
  void stop();
  void seek(qreal positionRatio);

  bool isPlaying() const;
  bool hasClip() const;
  int durationMs() const;
  int positionMs() const;

 signals:
  void positionChanged(int positionMs);
  void playbackFinished();
  void errorOccurred(const QString& errorText);

 private slots:
  void onPositionTimer();
  void onAudioStateChanged(QtAudio::State state);

 private:
  void cleanupDecoder();
  bool ensureAudioSink();
  void restartSinkAtCurrentPosition(bool startPlayback);
  void resetPlaybackBuffer();
  int bytesPerMillisecond() const;
  int alignedBytePosition(int bytePosition) const;
  int currentBufferPosition() const;
  int totalPlaybackBytes() const;

  OpusDecoder* m_decoder = nullptr;
  QAudioSink* m_audioSink = nullptr;
  QBuffer m_pcmBufferDevice;
  QByteArray m_pcmData;
  QTimer m_positionTimer;
  int m_durationMs = 0;
  bool m_started = false;
  // Consecutive position-timer polls during which the source has been fully
  // drained into the sink; used as a defensive fallback to end playback if the
  // played-position never quite reaches the reported duration.
  int m_idlePolls = 0;
  // Buffer byte offset at which the current sink run began (0 for a fresh play,
  // the seek target after a seek). Playback position = this + the audio the
  // sink has actually processed, so the reported position tracks real playback
  // instead of leading it by the (large, on Android) pull-mode buffer.
  qint64 m_playbackStartByte = 0;

  static constexpr int SAMPLE_RATE = 48000;
  static constexpr int CHANNELS = 1;
  static constexpr int FRAME_SIZE = 960;
  static constexpr int BYTES_PER_SAMPLE = sizeof(qint16);
  static constexpr int BYTES_PER_FRAME =
      FRAME_SIZE * CHANNELS * BYTES_PER_SAMPLE;

  // Silence prepended to the front of every clip's playback buffer. On Android
  // the first start() on a freshly created QAudioSink cold-starts the underlying
  // AudioTrack; during warm-up the pipeline under-runs and the opening of the
  // clip is emitted as silence and dropped (a bigger sink buffer alone did not
  // fix it -- the samples are lost, not merely delayed, which is why seeking
  // back later plays them: the track is warm by then). Playing back a run of
  // leading silence first means only silence lands in that window, so no audio
  // is lost. All reported/seek positions are clip-relative and hide the
  // lead-in (see positionMs/seek/currentBufferPosition), so timing and
  // scrubbing are unaffected. Desktop backends have no such cold start.
#if defined(Q_OS_ANDROID)
  static constexpr int LEAD_IN_BYTES = BYTES_PER_FRAME * 20;  // ~400 ms
#else
  static constexpr int LEAD_IN_BYTES = 0;
#endif
};